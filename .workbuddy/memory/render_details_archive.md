# venn —— 渲染特性实现细节（归档）

> 从 `MEMORY.md` 剥离出来的**实现级**细节。这些是「已经做完、且不会再动」的子系统说明，
> 查阅时才需要；日常改动前必读的**规则**仍在 `MEMORY.md`。
> 已完成的路线图项：R0-1 GPU 时戳 / R0-2 视图模式 / R1-1 TAA / R1-2 自动曝光 /
> R1-3 色彩分级 LUT / R1-4 动态分辨率 / R2-1 Hi-Z / R2-2 MDI / R2-3 Bindless /
> R3-1 阴影硬化 / R3-2 Froxel 体积雾 / R3-3 光源修饰 / R4-1 Alpha 模式 / R4-2 预积分 SSS。

## 天空盒 + 真 IBL（split-sum）
`render/Environment`（4 张图 + 采样器 + 描述符池 + 4 条 compute）+ `common.glsl` / `sky_cube.comp` /
`ibl_irradiance.comp` / `ibl_prefilter.comp` / `ibl_brdf.comp` / `sky.vert` / `sky.frag`。
- **天空模型唯一来源 `vennSkyColor(dir, zenith, ground)`**（`common.glsl`）。梯度**必须用 `dir.z`**
  （Z-up）—— 用 `dir.y` 会出现一条笔直的明暗带。
- **烘焙用 compute 而非 6 次 RP**：`vennCubeDir(face,uv)` 是纯函数，dispatch z 维=6 就写满。
- ⚠ **`imageStore` 不能指定 mip** → 预过滤每 mip 各建一个 `VIEW_TYPE_2D_ARRAY`（`baseMipLevel=m`）。
- 天空立方体 `RGBA32F`、BRDF LUT `RG32F`（为 CPU 回读免半精度转换）。irradiance/prefilter `RGBA16F`。
- **天空 pass 排在前向 Pass 最前**（depthTest/Write 全关）→ 排最后会盖掉地平面栅格。
- split-sum（Karis）：漫反射 = `albedo × irradiance(N)`（卷积乘 π，与 1/π 抵消）；
  镜面 = `prefilter(R, rough×maxLod) × (F0×A + B)`。
- **BRDF LUT 有解析解**：`(N·V=1, roughness=0)` 处**精确 A=1, B=0** —— 唯一能硬断言的检查点。
- 保留旧程序化近似路径，用 `FrameUBO.envParams.x` 切换（"两条路径给出不同画面"的对照手段）。

## 渲染性能三件套（详细规则）
- **视锥剔除**（`render/Frustum.h/.cpp`，Gribb–Hartmann；本工程 NDC z∈[-1,1]，近平面 `r3+r2`）：
  `uploadSceneState` 按**本场景**相机重建 `m_sceneFrustum`。开关 `frustumCullingEnabled()`/
  `MYVK_FRUSTUM_CULL=0`（关掉=全量提交，是"开关两次逐像素一致"的对照组）。
- **BVH**（`scene/Bvh`）：`spatialIndex()` 每帧一次，实体集合（含**顺序**）变了→`build`，否则 `refit`。
  渲染剔除与编辑器射线拾取**共用同一棵**。⚠ `query` 叶子必须**逐元素复测**（叶子盒是成员并集，
  并集相交 ≠ 每个成员相交）；`refit` 倒序遍历依赖"父下标恒小于子下标"；`buildRange` **只能重排
  order 下标表，不能动 items**。自检 `MYVK_BVH_DBG=1` 打 `BVH-DBG brute= bvh= missing= extra=`，
  `missing>0` = 会漏画的硬错误。
- **LOD**（`assets/Lod` + `AssetManager::ensureLod`）：`switchRatio[i]` = 进入第 i 级的上界
  （默认 {1.0,0.25,0.10,0.04}，迟滞 1.15）；比值 = `AABB半对角线/(距离×tan(fovY/2))`。
  `selectLevel(ratio,bias,current)` 里 **bias 是除**（>1 → 更快落低模）；`current<0` 时**从高模往下退**。
  程序化图元按细分减半重生成；导入网格走顶点聚类（格分辨率二分搜索 [2,256]）。
  **蒙皮网格不生成**（jointBase 按 `Mesh*` 记）。`MYVK_LOD_DBG=1` → `LOD-BUILD/STATS/SWITCH`。
- **Hi-Z / 遮挡剔除**：候选 AABB 表必须收**所有**视锥内物体；两帧延迟回读有时序不变量。
- **MDI（间接绘制）**：`vkCmdDrawIndexedIndirect` 不带 VB/IB → **必须自己绑**；提交组键必须含"网格"；
  "Pass 2.5" 要放 `recordFrame` 不是 `recordSceneInto`；`pbr.mdi.vert` 读的 binding 15 必须在 set 0
  layout 里声明。

## Framework 效果层（FrameGraph）
SSGI / SSR / GodRays / DoF / MotionBlur 走统一 FrameGraph 资源调度；新增效果必须登记依赖，
**关时不得录 pass**（否则基线不逐位一致）。
- 屏幕空间效果（SSAO/SSR/SSGI/god rays/DoF/MotionBlur/TAA）都要读 HdrTarget 的**深度图**。
  深度是 MSAA 的 → 多采样图像不能用 `sampler2D` 采样，必须 `sampler2DMS` + `texelFetch`
  （见 `assets/shaders/space.glsl` 顶部说明）。同一份源码编两份：
  `<名字>.<阶段>.spv`（`VENN_MSAA_DEPTH=0`）与 `<名字>.msaa.<阶段>.spv`（`=1`）。
  运行期按 `HdrTarget::samples()` 选。compute 版同理（`hiz_build.msaa.comp.spv`、
  `fog_inject.msaa.comp.spv`）。
- ⚠ `recreateScreenSpace()` 必须早于 `postInputViews()`。
- 多相机需每目标一份 FrameUBO。

## 分簇光照（实现细节补充）
- **固定槽位** `lightIndices[ci*cap+i]`（非原子追加）→ 结果**可复现**，截图对比才有意义。
- `maxLightsPerCluster` **唯一数据源是 C++**（经 `ubo.lightParams.w` 传片元），**禁在 pbr.frag 硬编码**。
  只花显存不花时间 → 默认 128。射灯只按 range 球剔除；衰减用窗口化平方反比 `win²/d²`，range 处严格归零。
- **光源修饰（R3-3）**：`LightInstance` 保持 64B 不动，修饰走**并行 SSBO** `LightExtInstance`
  (32B, binding17)，**下标与 `lights[]` 严格对齐**（`Scene::collectLights(out, mods*)` 同一循环 push）。
  三修饰 = IES（角度剖面）/ cookie（锥内投影）/ 区域光（球光代表点 `r=√(hw·hh)`）。
  ⚠ **基线纪律**：三开关全关 → 记录全零 → 着色器走**原样的** `shadeLocalLight` 分支
  （不是"新函数里 mod=1"）——浮点重结合会让最后一位不同，破坏"关=基线逐位一致"。
  `LightTextureLibrary` 是 cookie+IES 共用的 `sampler2DArray`（256²、16 层、`R8G8B8A8_UNORM`，
  第 0 层恒全白，内建 white/cookie:grid|rings|bars/ies:narrow|wide|batwing，支持 LM-63 Type C 解析）。
  `.ies` 头行要用 `std::streamoff` 存 `tellg`（`std::streampos` 不能直接赋 0）。
- 透明（Blend）不写深度、按相机距离**降序**排序、**不参与深度预通道**。
- 遥测 `stats` SSBO：0 单簇最大/1 溢出簇数/2 配对数/3 空簇数（`beginFrame` 回读并清零）。
- 每目标多相机：切簇网格与 HdrTarget 尺寸时都要同步更新 `HiZOcclusion` / `VolumetricFog` 的内部尺寸
  （见启动日志里的 `ClusteredLighting: ... clusters` 与 `HiZOcclusion: ... mips` 成对出现）。

## 视口像素断言（补充工具性细节）
- `region_diff` 按 stride 2 抽样。
- 验证脚本的辅助设施见 `MEMORY.md` 的「工具」节。

---

# 阴影（实现细节 · 2026-10-14 从 MEMORY.md 整体搬入）

## 方向光 CSM（`ShadowPass`）
1. **投影矩阵必须 Zero-to-One**：`glm::orthoRH_ZO`（不是 `glm::ortho`）。Vulkan clip z∈[0,1]，
   GL 的 [-1,1] 会让深度图与 `pbr.frag` 参考深度不在同一空间 → 覆盖区一片黑。
   `pbr.frag::shadowFactor` **只对 xy** 做 `*0.5+0.5`，z 直接用。
2. **每级正交范围 = "该级视锥切片的包围球"**（`invViewProj` 反投影 8 角点求球），**不是物体 AABB**
   （球半径与光方向无关；盒会让光一转阴影边缘"呼吸"）。球心在**光轴坐标系里量化到纹素网格**
   （`MYVK_SHADOW_SNAP=0` 可关，默认开）。⚠ **量化必须作用在球心的世界位置**：用只含旋转的光轴基
   `(lx,ly,lz)` 把 `mid` 投影后取整，再 `center = mid + lx*(sx-ax) + ly*(sy-ay)` 重建 lookAt。
   **不能对 `view*mid` 量化**（`mid` 是 lookAt target，`view*mid≡(0,0,-dist)`，量化 = 空操作）。
   正交**近平面取 0**，靠 `kDepthPadding = 25m` 外扩。退化时 `radius` 下限 0.05。
3. **只接收不投射的物体必须 `VisibilityComponent::castShadow=false`**（自带地面就是）。
4. **级联分割 PSSM practical**：`λ·near·(D/near)^(i/N) + (1-λ)·(near+(D-near)·i/N)`，λ=0.6。
   **near/far 必须从 `camera().nearPlane()/farPlane()` 读**（级联分割、切片角点 NDC 换算、
   分簇对数切片三处假设同一组值）。D=90/near=0.1 → `9.36 / 19.82 / 36.87 / 90.00`
   （`tools/verify_csm.py` 逐项对公式）。级联盒**边缘不淡出**（只在"视深度 > 最大距离"处
   `smoothstep`），否则交界处假暗带。
5. **R3-1 阴影硬化三件套**（`SHADOW_BLEND` / `SHADOW_HARDEN` / `SHADOW_SNAP`）：
   `FrameUBO.shadowFilter = (blendBand, harden, 0, 0)`。① **级联间混合带**：接收点落在"本级远端
   最后 band 比例"内时，同时采样下一级并按深度插值。② **接触硬化**：按接收点在级联内的相对视深度
   缩放 PCF 核半径。③ **texel snapping**：见第 2 条。每件都"默认关 = 逐像素等于基线"，每件都要 A/B。
   ⚠ **snap 的效果无法用截图验证**（亚像素级）→ 断言 `MYVK_SHADOW_MATRIX_DBG=1` 打的
   `SHADOW-MATRIX c0 t=(..)`。见 `tools/verify_shadow_hardening.py`。

## 局部光源（点光/聚光，`LocalShadowPass`）
1. **近平面按 range 取 `clamp(range/20, 0.1, 2.0)`**。透视 NDC 深度是**双曲**的：
   range=32 用 near=0.05 时，"离灯 15m 遮挡体"与"20m 地面" NDC 只差 7.5e-4 → 阴影整个消失。
2. **深度偏移以「米」给，片元按参考深度换算回 NDC**（`LightShadowRecord.params=(near,far,灯下标,
   有效位)`；`pbr.frag::ndcBiasFromWorld`：`dz/dd = (f − z·(f−n))² / (n·f·(f−n))`）。照抄 CSM 的
   NDC 常数在 range=32 下等于 10 米世界深度。**别给双曲深度配 NDC 常数偏移**。
3. **未使用的层每帧也要走一次空 depth-only pass（只清深度）**：整张 array 被采样器一起绑定，
   留一层停 UNDEFINED → 验证层报 IMAGE_LAYOUT。
4. **层分配：按到相机的距离平方由近到远贪心，预算不够就整体停在那一盏之前**。点光 6 层/聚光 1 层，
   `kMaxLocalShadowLayers=12`；结果写回 `LightInstance::cosOuterType.z`（基准层号），点光实际层号
   = 基准 + `vennCubeFaceIndex(dir)`。光栅化 `depthBias` 对浮点深度几乎为 0，真正偏移在片元侧。

## 两套阴影共用
- `ShadowPass`/`LocalShadowPass` 共用 `shadow.vert`，靠 `ShadowPushConstants.which.y` 选矩阵来源
  （0→`FrameUBO.lightSpace[x]`，1→set0/binding11 `lightShadow.records[x].matrix`）。**矩阵只有一份**。
- `ShadowCull.h::visibleInLightClip` 是两套阴影共用的局部 AABB 剔除（正交/透通用，`w≤0` 放弃剔除）。
- `ShadowPass` 推常量是 `ShadowPushConstants`（96B，非主前向的 128B）—— 因为它在**自己的 RP 内自行
  绑 set 0**。**绝不要把该 Pass 绘制搬进前向 RP**，否则 VUID-08600。
- **array 深度图渲染**：framebuffer/`imageStore` 不能挂 array 视图做单层渲染（那是 layered
  rendering）。每层单独建 `VIEW_TYPE_2D` 视图 + framebuffer；采样侧才用 `sampler2DArrayShadow`。

---

# editor/ 编辑器层（实现细节 · 2026-10-14 从 MEMORY.md 整体搬入）

三条铁律：**① 编辑器模式不跑游戏逻辑 ② 渲染目标是 ImGui 子窗口而非全屏 ③ 编辑态与运行态分离**。
- **运行态 = 同进程的内存副本**（`m_runtimeScene`）：`EditorContext::activeScene()` 是唯一分派点。
  Play = 编辑态 `sceneToJson` → `sceneFromJson(m_runtimeScene)`（**内存拷贝，不落盘不起进程**）。
  **不做跨进程**。运行态相机 `play()` 里对副本调 `setUseGlobalInput(false)`+`setFlyMode(true)`。
  `pause()` 只翻状态；`stop()` 丢弃 runtime scene、复位 `m_gameFullscreen`、`validateSelection()`。
- **Play 期间编辑器只读**（gizmo/Inspector/层级增删被 `!isEditing()` 挡住）。
- **命令模式**：值编辑 `RawBytesEditCommand`，结构编辑 `SceneSnapshotCommand`（整场景 JSON）；
  统一入口 `structuralEdit(name,fn)`。破坏性操作**两段式**（ECS 遍历期间不能增删 → 攒 pending 队列，
  树画完 `flushPending()`）。
- **视口解耦**：`setViewportSize` 让 3D 输出写离屏图，`viewportTextureId()` 给 `ImGui::Image`。
  纹理稳定性：① recreate 保留场景输出图 ② `createViewportTextures` 比较未变则不重注册
  ③ ViewportPanel 尺寸刚变那帧不画图（`m_resizePending`）。
- **布局**：SplitLayout 二叉分栏树；主体面板必须 **Always 落位 + NoMove/NoResize**；
  标题带状态须加稳定 ID（`###viewport`）。
- **自研脚本 .vks**（`editor/script/ScriptEngine`）：`ScriptHost{scene,entity}` 是唯一接缝；
  热重载 mtime 轮询 0.5s，**编译失败保留上一版**；数值用 double，`rotate` 单位是**度**，
  步数上限防死循环。只在 Play 态跑且只作用于 runtime scene。
- **全局观感走 `ImGuiManager::applyVennStyle()`**：accent `(0.16,0.53,0.94)`、
  `WindowBg(0.106,0.110,0.118)`、`MenuBarBg == TitleBg(0.113,0.117,0.125)`。
- **面板宽度自适应**：`kLeftFrac 0.175/min300/max520`、`kRightFrac 0.205/min330/max560`；
  用户拖过分隔条后 `m_userAdjusted` 不再自动重算。
- **快捷键**：`F2` 重命名 / `Ctrl+D` 复制 / `Ctrl+N` 新建场景 / `Delete` 删除 / `Backspace` 上一级。
- **Content 右键菜单是"显式单 popup"**：只写 `m_ctxRequest/m_ctxEntry`，循环外 `OpenPopup` +
  `drawEntryContextMenu`；`BeginPopup` 返回 false 就自动清状态。

### 关键子系统
- **Shift+A 添加菜单 / 播放条 / Preferences**：菜单视口 hovered 才触发，popup 每帧无条件提交；
  落点 = 鼠标射线∩z=0。**Sunlight = 场景级太阳**（唯一、有阴影、不进实体序列化）；
  **Directional Light = 实体级补光**（`Type::Directional=2`，`positionRange.w=1e5` 影响球 trick）。
  播放条 `LayoutRects.transport`，视口底部扣 `kTransportH=34px`。Preferences 窗口 ID 是 `###prefs`。
- **编辑器锁定**：`ecs::LockedComponent`（纯标记）。`PickingSystem::pick` 跳过它、
  `GizmoController::update` 直接 return。序列化字段 `je["locked"]`（**读写都要补**）。自带 `Ground`
  默认带它。数字键 0 = 落地（只改 z、世界包围盒底部贴 z=0）。
- **碰撞体系统**（`engine/physics/` 四文件 + `editor/ColliderView`）：QuickHull 必须配冲突表；
  GJK+EPA 不依赖绕序。`CollisionComponent` 与 `TransformComponent` 字段名/类型/欧拉序完全一致。
  序列化只存 7 参数，凸包顶点读盘后按 mesh 重建。线框走 ImGui draw list。**编辑态碰撞不阻止变换，
  只在运行态分离**。`MYVK_EDITOR_COLLIDER=hull|capsule` 自动化钩子。
- **Player 系统**：实体 = 玩家"脚"（贴地 root），`PlayerComponent{mode, eyeHeight=1.7, moveSpeed=3.0,
  lookSpeed=0.0025, yaw, pitch, camDistance=3.5, turnSpeed=12, walkPhase, bodyYaw, started}`。
  第一人称 yaw/pitch = 视线；第三人称 = 轨道方位/俯仰，角色朝移动方向转身（最短角差 wrap 插值，
  `rotation.z = -bodyYaw`，Z-up）。⚠ **Play 态 W/E/R 手柄快捷键必须 `isEditing() && !flying` 守卫**。
- **FBX 导入**（ufbx）：`ufbx_matrix` 是 3x4（12 float），memcpy 到 mat4 会越界 → 按列构造。
  **蒙皮核心约定**：蒙皮顶点加载时**烘到静止世界空间**，`invBind(j)=inv(W_rest(j))`、`invMeshNode=I`。
  模型归位：世界 AABB 中心平移回原点（UE 惯例）。**资产端坑**：Blender 导 FBX 声明 Z-up 但数据沿
  +Y → 导出前把 armature 转 90°X；`bake_space_transform` 别用。改 assets/models/*.fbx 后必须手动 cp
  到 build/bin/assets/models。CharacterWindow 的 `recordPreview` 必须在 **ImGui 之前**。
- **Hierarchy 文件夹节点**：显式标记 `HierarchyComponent::group`。树的图标不能塞进 `TreeNodeEx`
  label → label 传空串，图标+名字用 DrawList 自己画（`行左边缘 = GetCursorScreenPos().x`
  **必须在 TreeNodeEx 之前取**）。

---

# 视口相机与导航（实现细节 · 2026-10-14 从 MEMORY.md 整体搬入）

两者存储模型相同（`scene::Camera` 自由飞行），但归属完全不同：
- **编辑态 = 编辑器视口导航相机**，编辑器完全驱动。
- **Play 态 = 游戏相机**，编辑器**不碰**（视口在 Play 期间不响应任何键鼠导航）。

编辑态导航（按 UE）：右键拖=自由旋转 / 中键拖=平移 / 单独滚轮=推拉相机（`kNavDollyPerNotch=0.6`）/
右键+滚轮=灵敏度 / 右键+WASD=飞行（**只在按住右键时**）/ 右键+Q/E=升降（Shift ×3）/ 左键=点选+gizmo /
左下导航球 / 数字键 1/3/7=Front/Right/Top（Ctrl 反向）。
- **滚轮两条路靠"是否按住右键"分流**（复用 `lookDown`，无第三种语义）。
- **"飞行只在按住右键时生效"是必须的**：`W/E/R` 同时是手柄快捷键，用 `IsMouseDown(Right)` 门控。
- `applyEditorCameraMode()` = `setUseGlobalInput(false)`+`setFlyMode(true)`。**每次场景重建必须重设**
  （新建/打开/重建 demo → `onSceneReplaced()`；undo/redo → 各自入口显式调用）。**飞行模式不在序列化
  范围内**，漏掉表现为"导航突然变回老式轨道相机"。
- **导航球**（`ViewportPanel::drawNavGizmo`，Blender 风格）：整块方形是 `InvisibleButton`，
  四角也吞鼠标。切视图/拖拽都**绕当前注视点**转。Z-up 映射：
  Front=-Y(yaw=+π/2)/Back=+Y/Right=+X(yaw=0)/Top=+Z(pitch=+kPitchLimit)。数字键接入点必须在
  `io.KeyCtrl` 分支**之前**。主键盘数字键是 `ImGuiKey_1`。`Camera::kPitchLimit = 1.5533f`（89°）。
- **Camera 飞行模式不变式**：`position()` 与 `m_target` 通过 `target = position - angleDir()*distance`
  **互为逆运算**。**任何动 target/distance/yaw/pitch 的接口都必须同步另一侧**。相关入口：
  `syncFlyTarget()` / `setTarget` / `setDistance` / `setYaw` / `setPitch` / `orbit` / `zoomBy` /
  `panWorld` / `dolly` / `update()`。新增接口先问"飞行模式下这条还成立吗"。
- **灵敏度浮层**（`m_navSensitivity`，默认 1，clamp 0.05..20）只作用于编辑态。**浮层必须是 viewport
  的 child window**；**浮层尺寸必须按字体度量算**。

## Gizmo 拖拽与吸附
**两个曾经的致命缺陷（用户报"拖拽方向反 + 越拖越快"），已修，改这里前必读：**
- **① 轴线/射线最近点的参数符号**（`PickingSystem::closestOnAxis`，Ericson 5.1.9）：
  参数用 `(c*d - b*e) / (b*b - a*c)`，分母只用于判平行。**改符号前先想清楚分子分母是不是同时翻了。**
- **② 拖拽参考线必须"冻结"**（正反馈环）：`beginDrag` 快照 `m_dragOrigin/m_dragAxis/m_dragWorldLen`，
  `applyDrag` 全程只用快照。用每帧当前位置会指数发散。
- **三套独立吸附**（量化语义各不相同）：平移=**位移增量**（默认 0.1m）/ 旋转=**角度增量**（10°）/
  缩放=**倍率 factor**（0.1）。UI：视口工具栏第二行 `drawSnapRow()`，**G 键**切换（`!flying` 内）。
  旋转**始终在"拖拽起始姿态"上叠加**。
- **手柄 7 种 `Handle`**：`AxisX/Y/Z`、`PlaneYZ/XZ/XY`（**枚举名 = 该平面法线**，颜色按法线取）、
  `Screen`。平面方片命中用 `pointInQuad()`；平面拖拽**必须兜底**（`kMaxDragUnits=2000`）。
- **本引擎欧拉序 `R = Rx·Ry·Rz`**，而 `glm::eulerAngles` 用 YXZ，**不能直接用** → 自己写
  `eulerFromRotation()`（`y=asin(m[2][0])`、`x=atan2(-m[2][1],m[2][2])`、`z=atan2(-m[1][0],m[0][0])`，
  `|sy|>0.99999` 走万向锁分支）。
- **缩放：拖的那根轴 ≠ 改的那个分量**。修法：把拖拽方向**投影到局部轴**再分配
  `sc[i] = m_startScale[i]*(1+(factor-1)*w[i])`，`w[i]=|dot(axis,localAxisWorld[i])|`。

## 地平面参考栅格（`grid.{vert,frag}` + `Renderer::GridSettings`）
- **解析式、零顶点缓冲**：VS 用 `gl_VertexIndex` 拼 4 顶点铺在 z=`planeOffset`；FS 按
  `fract(p/step+0.5)-0.5` 求到最近线的距离，除 `fwidth(p)` 转**像素距离** → 1px 抗锯齿。
- **三种图层**：最小格(1m 暗) → 主格(10m 亮，线宽×1.4) → 世界轴（**X 红 / Y 绿**）。默认 extent60/1/10。
- **自带地面**（`resetToEmptyScene` 建 `Ground`，`castShadow=false`，顶面 z=0 与栅格面重合）。
  改地面尺寸/位置时**必须同步 `GridSettings::extent`**。地面是"上锁"的。
- **淡化是"距栅格中心"不是"距相机"**（用切比雪夫距离）。**线宽必须按占空比封顶**
  （`kMaxLineDuty=0.22`）；亚像素淡出阈值 `smoothstep(0.7,1.6,cellPx)`。与地面共面压 z-fighting：
  VS 里 `clip.z -= 1e-6*clip.w`。
- 管线：TRIANGLE_STRIP/无顶点布局/depthTest=true/depthWrite=false/blendEnable=true/cullMode=NONE。
- **开关**：`gridEnabled()` 由 `EditorApp::onUpdate` 每帧写 `= isEditing() && ctx.showGrid()`
  → **Play 期间强制关**。UI 三处等价：视口 HUD 勾选框 / View 菜单 / `MYVK_GRID=0`。

## 全屏与窗口几何
- **Play 态全屏** = 窗口真全屏 + 视口独占整屏：`m_gameFullscreen` 只存逻辑状态，
  `EditorApp::syncWindowFullscreen()` → `Window::setFullscreen(true)` → `glfwSetWindowMonitor`。
  `F` 切换 / `Esc` 退出（都只在 Play 态）。**F11 = 只切窗口全屏**（任何模式可用）。切换**必须做边沿
  检测**（`m_appliedWindowFullscreen`），否则每帧无条件 set 会立刻关掉 F11 全屏。
- **窗口几何**（`core/WindowGeometry`）：无存档 → 工作区居中、宽高=工作区×0.70；有存档 → 用存档
  （含 maximized），**必须先做可见性校验**。`onInit()` 第一件事落位、`onShutdown()` 保存。
  自动化跑不写存档（`MYVK_FRAMES`/`MYVK_NO_WINDOW_SAVE` 存在时跳过）。

## venn 命名与图标
窗口标题 `Venn Editor`，日志头 `=== Venn Engine - Editor ===`。图标是**滑稽可爱豚鼠**。
三层独立缺一不可：① exe 资源（`venn.rc` + `enable_language(RC OPTIONAL)`，`.ico` 七档）② 窗口图标
（`GLFW_VISIBLE=FALSE` 建窗 → `applyIconFromResource()` → 再 show；三处设 `WM_SETICON` +
`SetClassLongPtrW`）③ AUMID（`SetCurrentProcessExplicitAppUserModelID`，必须在任何窗口创建之前）。
> ⚠ **不要**退回"单张 512 PNG + `glfwSetWindowIcon`"（GLFW 只发 `WM_SETICON`、不设窗口类图标）。

## 资产溯源与序列化
- Mesh/Material/Texture 各带"来源描述"，序列化只写描述、加载时由 AssetManager 重建；路径入 JSON 前
  过 `makeAssetRelative()`；层级父子用**数组下标**；相机与方向光也在同一 JSON。
- **`Mesh` 带局部 AABB 缓存**（`hasBounds()/boundsMin()/boundsMax()`，`upload()` 时算一次）：
  阴影正交范围、编辑器拾取/贴地都要"物体实际尺寸"。
- **`Texture` 色彩空间是契约**：`fromPixels(...,srgb)` / `makeSolid(...,srgb)` —— albedo/emissive
  用 true，**normal/ORM 必须 false**。
- **材质 6 个纹理槽**：albedo/normal/orm/**roughnessMap/metallicMap/emissiveMap**。roughness/metallic
  **相乘**（默认纯白=恒等）；emissive **相加**（默认**纯黑**=恒等）。描述符 `kTextureSetCount 6`，
  setLayouts 7。落盘的 `name` 就是纹理**缓存键**，数据贴图的 `#linear` 后缀**不能剥**。
- **程序化贴图 JSON**：`TextureSource::Kind` 只有 `Solid`（键 `color`，RGBA 4 分量）/`Checker`
  （键 `colorA`+`colorB`+`cell`）/`File`。**没有 `alpha`/`pattern` 键**（写错会被当不存在）。
- **场景 JSON 的 mesh `size` 只吃标量**（数组会让加载失败）。

---

# R4-1 Alpha 模式（实现细节 · 2026-10-14 从 MEMORY.md 搬入）

- **`Mask` 的 alpha-to-coverage**：MSAA 下（`samples != 1`）开 a2c，并把 alpha 预映射成
  `alpha' = clamp((alpha - cutoff) * 2 + 0.5)` —— 让"alpha == cutoff"正好落在 a2c 硬编码的 0.5
  阈值上，使 cutoff 滑条在 1x 与 MSAA 下表现一致。⚠ **1x 下开 a2c 会让 `vkCreateGraphicsPipelines`
  直接报错**，必须用 `samples != 1` 守卫。
- **`Mask` 为什么不进深度预通道**：预通道不采样材质贴图，无从判断镂空 → 让它进预通道会让
  alpha 裁切失效（深度被实体面写满）。
- 抖动裁切成 y 有序抖动（Bayer 4×4）而不是丢弃：**照样写深度**，配 TAA 把抖动边转成柔和边。

# R4-2 预积分皮肤次表面散射（实现细节 · 2026-10-14）

## LUT 的物理模型与生成（`tools/make_sss_lut.py` → `assets/textures/sss_lut.dds`）
- 256×256，**BC1 + 完整 mip 链**，`srgb=false`（UNORM）。列 = `N·L ∈ [-1,1]`，行 = 曲率 `∈ [0,1]`。
- 逐通道散射核积分（复用 `make_bc_dds.py` 的 `encode_bc1` / `mip_chain` / `write_dds`）：

      D(cosθ, curv) = Σ_φ P(s) · max(0, cos(θ − φ)) / Σ_φ P(s)
      P(s) = exp(−s² / 2σ²)     s = r·φ     r = 1/curv     σ = (500, 300, 150) mm

  φ 取 512 个样本，`curv` 下限 `CURV_EPS=1e-4`，`MM=1000` 把"世界单位"当米。
- **逐行逐通道归一化**（除以 `N·L=1` 处的值）是必需的：不归一化时正面 N·L=1 处会被压到 0.88。
  归一化后**曲率=0 的行逐点等于 `max(N·L, 0)`**（误差 1.1e-16），即平面自动退化成 Lambert。
- 自检数值（供验证脚本 A 组引用）：`lut[255, N·L=0] = (0.2061, 0.1196, 0.0581)`、
  `lut[255, N·L=1] = (1,1,1)`；固定 N·L=0.3 时 R 随曲率单调 0.294 → 0.385。
- 同时输出 `sss_lut.png` 供肉眼查看。

## 着色器接线
- `pbr.frag`：`skinDiffuse(N, L)` 采样 LUT；曲率来自 `length(fwidth(几何法线)) * ubo.sssParams.x`。
  **几何法线必须在法线贴图扰动之前取** —— 用扰动后的 N 算 `fwidth` 会让贴图高频细节整片爆成噪点。
- 三处调用点：方向光主 BRDF、`shadeLocalLight`、`shadeLocalLightMod`（局部光的漫反射乘子）。
- `Renderer::createDefaultTextures()` 里加载 LUT；**加载失败 → 程序化生成一张 256² 的 Lambert 表兜底**
  （R=G=B=clamp(N·L,0,1)），功能不生效但绝不黑屏。`writeGlobalSet` 里 binding19 兜底绑 `m_defaultWhite`。
- `MYVK_SSS=f` 覆盖 `m_sssCurvatureScale`（默认 `kSssDefaultCurvatureScale = 8`）；经
  `FrameUBO.sssParams.x` 传给着色器。

## ⚠ 采样边界缺陷（本轮最贵的一课）
- **现象**：着色器里 `textureLod(sssLut, vec2(0.55, 0.0), 0.0).r` 读出 ≥ 0.18，而脚本算出来是 0.098；
  且 `v=0.0` 与 `v=1.0` 读出**同一个值**。更早的症状是 "Skin(scale=0) vs Default" 差 **5.614**。
- **根因**：`Texture::createSampler` 的地址模式是 **REPEAT**，而两条轴的端点恰好落在纹理边界上
  （`u=1` ← 正对光；`v=0` ← `fwidth(几何法线)=0` 的平坦表面）。边界处双线性会跟"绕到另一侧"的
  纹素各取一半 → 曲率 0 的行掺进曲率 1 的红边，**皮肤所有平坦区域无端泛红**。
  `(0.098 + 0.260)/2 = 0.179` 正好解释那个 0.18。
- **修法**：`uv = (vec2(u, curv) * (N-1) + 0.5) / N` 把坐标钉到**纹素中心**，与 `linspace` 布点逐格
  对齐、永不跨界。修复后 "Skin(0) vs Default" **5.614 → 0.204**（27 倍）。
- **否定测试**：把坐标改回 `vec2(u, curv)` 重新构建 → B4 立刻回到 **5.6143**，证明断言非空。
- **误导性**：这个 bug 与"构建系统拷贝陈旧 shader"缺陷叠加时，表现为"同一个旋钮时而有效时而无效"，
  极难定位。**先排除构建陈旧，再怀疑算法与采样。**

## 验收实测（`tools/verify_sss.py`，球 + 强侧光）
| `MYVK_SSS` | 球区平均亮度差 | ΔR | ΔG | ΔB |
|---|---|---|---|---|
| 0（vs Default） | 0.2041 | — | — | — |
| 1 | 0.028 | +0.03 | +0.03 | +0.02 |
| 4 | 0.514 | +1.07 | +0.38 | +0.08 |
| 8（引擎默认） | 1.924 | +3.95 | +1.46 | +0.34 |
| **16（测试取值）** | **6.411** | +13.46 | +4.79 | +0.95 |
| 24 / 32 / 48 / 64 | 10.34（**饱和，逐位相同**） | +21.5 | +7.9 | +1.6 |

`MYVK_SSS=0` vs `=16` 时 Default 球逐像素差 **0.000000**（Default 完全不引用 SSS 路径）。
⚠ **≥24 后曲率被 clamp 到 1 而饱和** → 对照组必须选在敏感区间（这也验证了
"全有/全无的开关测不出分级"这条纪律）。

---

# 构建：资源拷贝"陈旧副本"缺陷（2026-10-14 定位并修复）

- **症状链**：改 `pbr.frag` → `make build` → `build/shaders/*.spv` 时间戳**更新了**，但
  `build/bin/shaders/*.spv` **不变** → 引擎跑的还是旧着色器。连续多轮探测（probe6~probe19）
  测的全是旧代码，导致大量自相矛盾的观测（`MYVK_SSS=0` 与 `=40` 逐位相同、硬编码常量却"随环境变量变化"）。
- **根因**：`.spv` / 贴图 / 模型 / 字体的拷贝写成了 `add_custom_command(TARGET Editor POST_BUILD)` ——
  它**只在 Editor 目标重新链接时**执行；改着色器不会让 Editor 重链接（`add_dependencies` 只保证顺序，
  不触发重链接）。
- **修法**：改成 `add_custom_target(Editor_shaders ALL ... DEPENDS ${SHADER_TARGETS})` +
  `add_dependencies(Editor Editor_shaders)`；`assets/models|textures|icons` + imgui 字体挂
  `Editor_assets`（`ALL`）目标。**必须用 `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}` 输出**，
  用 `$<TARGET_FILE_DIR:Editor>` 会让 `Editor` ↔ `Editor_shaders` 形成目标循环依赖、CMake 直接报错。
- **保留 POST_BUILD 的**：`assets/scenes`、`assets/scripts` —— 这两个目录是**编辑器运行时会写**的，
  每次构建都覆盖会把用户的场景改回去。
- **确诊手段**：往 `pbr.frag` 里写一句常量红 `outColor = vec4(1,0,0,1)`，重跑看球心是否真的变红。
  这是唯一能证明"我改的代码真的在跑"的办法。
- **日常检查**：`cmp assets/shaders/pbr.frag build/bin/shaders/...` / `cmp assets/textures/x.dds
  build/bin/assets/textures/x.dds`。

---

# 全局 set0 描述符表 / 构建路径表（查阅用 · 2026-10-14 从 MEMORY.md 搬入）

## `set 0` 的 25 个 binding（`kGlobalSetBindings = 25`）
| # | 内容 | 备注 |
|---|---|---|
| 0 | FrameUBO（**768B**） | 唯一来源 `RenderTypes.h`；`static_assert(sizeof(FrameUBO)==768)`，末尾 `probeParams` @752 |
| 1 | shadowMap | `sampler2DArrayShadow`，第 i 层 = 第 i 级级联 |
| 2 | lights | `LightInstance[]` |
| 3 | clusters | 簇网格 |
| 4 | lightIndices | 固定槽位 `[ci*cap+i]` |
| 5 | clusterStats | 遥测 SSBO |
| 6 | jointMatrices | 蒙皮 |
| 7 | irradianceMap | IBL |
| 8 | prefilterMap | IBL |
| 9 | brdfLut | IBL |
| 10 | skyMap | 天空盒 |
| 11 | lightShadows | `LightShadowRecord[]` SSBO（局部阴影矩阵） |
| 12 | localShadowMap | `sampler2DArrayShadow`，12 层 |
| 13 | instanceModels | vertex 阶段，instancing |
| 14 | ssaoTex | R8，关时绑白图 |
| 15 | mdiInstances | vertex 阶段，MDI |
| 16 | vfogTex | `sampler3D`，关时绑 1×1×1 透明图 |
| 17 | lightExt | `LightExtInstance[]`，**下标与 binding2 严格对齐** |
| 18 | lightTex | `sampler2DArray`（cookie + IES 共用），第 0 层恒全白 |
| 19 | sssLut | `sampler2D`，R4-2 皮肤预积分 LUT（BC1 + mip，UNORM） |
| 20 | decalAlbedo | R4-4 DBuffer 反照率（`R8G8B8A8_UNORM`），**关/无贴花时绑 `m_defaultTransparent`** |
| 21 | decalNormal | R4-4 DBuffer 世界法线（同格式 / 同缺省） |
| 22 | decalRoughMetal | R4-4 DBuffer 粗糙度-金属度（同格式 / 同缺省） |
| 23 | probeMap | R5-1 反射探针立方体数组 `samplerCubeArray`，**关/无探针时绑 1×1 全黑 cube array（6 层）兜底图** |
| 24 | ProbeBuffer | R5-1 探针参数 SSBO（`ProbeGpu`，std430 **112B** = mat4 + 3×vec4，无尾填充） |

⚠ **DBuffer 的缺省图必须是 `m_defaultTransparent`（RGBA 0,0,0,0）**，**不能**用
`m_defaultBlack`：DBuffer 第四通道是 coverage，alpha=255 会让整屏被当成"coverage=1 的黑色贴花"。

⚠ **探针 binding 23 的兜底图不能省**（不是"关着就不绑"）：`pbr.frag` 声明了该 binding，
只要 layout 里有这个资源，创建**任何**用 `pbr.frag` 的管线时都必须有有效描述符，
否则 `VUID-VkGraphicsPipelineCreateInfo-layout-07988`（与 SSAO 缺省白图 / 贴花全透明图同一条约定）。
⚠ GLSL 侧 `struct Probe` 必须定义在 interface block **之外**（结构体不能定义在块里）。

灯 / 环境 / 雾 / 光源修饰 / SSS / **贴花 DBuffer** / **反射探针** 全挤在 set 0：Vulkan 只保证 4 个 set 同时绑定，
而 set 1..6 已被六张材质贴图占满（set7 被 POM 的 heightMap 占了）。新增 binding 必须同步
`kGlobalSetBindings` + `writeGlobalSet` + 着色器 layout 三处。

## 本机构建路径
| 项 | 值 |
|---|---|
| VS2022 Pro | `D:\Microsoft Visual Studio\2022\Professional`（MSVC 14.44 / cl 19.44，Win SDK 10.0.26100） |
| cmake | 4.4.3 |
| ninja | `C:\Users\wumen\bin` |
| GNU Make 4.4.1 | `C:\ProgramData\chocolatey\bin\make.exe`（`VENN_MAKE` 可覆盖） |
| Vulkan SDK | `D:\A-VENN\code\3d-render\VulkanSDK\1.3.296.0` |
| MinGW / gcc / g++ / mingw32-make | **整条缺失**（所以 `TOOLCHAIN=auto` 只能走 MSVC） |
| 工程根 | `D:\A-VENN\code\cpp\venn-engin`（CMake 工程名 `Venn`，刻意不一致） |

支撑脚本：`vcvars.cmd`（搭 MSVC 环境，**不调 vswhere / vcvars64.bat**——后者会 shell out 到 `reg.exe`）、
`msvc-build.cmd`（MSVC 构建驱动器）、`make.cmd`（自动找可用的 GNU Make）。

## 其它构建约定
- 手动 CMake（不走 `make`）：MSVC 侧**必须先有 vcvars 环境**
  （`cmd /c "vcvars.cmd && cmake -S . -B build -G Ninja ..."`）；MinGW 侧才需要显式
  `-DCMAKE_C_COMPILER=` / `-DCMAKE_CXX_COMPILER=`（路径**必须整体加引号**）。
  `"-DCMAKE_POLICY_VERSION_MINIMUM=3.5"` **两条路都必须**（CMake 4 拒绝 GLFW 的 3.4）。
- 依赖版本锁：**tinygltf 固定 v2.9.4**（pin `5f330f3952`；master 已是 v3，API 不兼容）；
  imgui / stb / tinyobjloader / VulkanMemoryAllocator 用 master；glfw 3.4；glm 1.0.1。
- `build/bin/imgui.ini` **会在下次启动覆盖默认面板布局**（自动化跑必须 `io.IniFilename=nullptr`）。
- glslc 输出到 `${CMAKE_BINARY_DIR}/shaders`；`CMAKE_RUNTIME_OUTPUT_DIRECTORY = ${CMAKE_BINARY_DIR}/bin`。

---

# 环境变量全表（自动化钩子 · 2026-10-14 从 MEMORY.md 搬入）
> 只在设了时生效。`MYVK_EDITOR_CAM` **必须排在 `MYVK_EDITOR_SCENE` 之后**（坑#46）。
- **基础**：`MYVK_FRAMES=N`(跑 N 帧退出+抑制窗口存档) / `MYVK_VALIDATION=0` / `MYVK_SHADER_DIR` /
  `MYVK_ASSET_DIR` / `MYVK_HIDE_UI=1` / `MYVK_NO_WINDOW_SAVE=1` / `MYVK_CONTENT_DIR` /
  `MYVK_FREEZE_ANIM=1` / `MYVK_SCENE_TEST=1`。
- **视口像素断言必备**：`MYVK_EDITOR_NO_HUD=1`（关掉视口全部 2D 叠加层；里面的 FPS/帧时间
  每次运行都不同，不关掉"两次逐像素相同"永远失败）。另 `MYVK_GRID=0`（栅格是屏幕空间固定图案）。
- **编辑器**：`MYVK_EDITOR_SCENE=<路径>` / `MYVK_EDITOR_SELECT=<名字>` /
  `MYVK_EDITOR_GIZMO=move|rotate|scale` / `MYVK_EDITOR_PREFS=1` /
  `MYVK_EDITOR_CAM="x,y,z,yawDeg,pitchDeg"` / `MYVK_EDITOR_SPACE=world|local` /
  `MYVK_EDITOR_COLLIDER=hull|capsule` / `MYVK_CHARACTER_MODEL=<路径>`。
- **渲染**：`MYVK_MSAA / LIGHTS / CLUSTER_CULL=0 / DEPTH_PREPASS=0 / LIGHT_GIZMOS=0 / TILE_SIZE /
  CLUSTER_SLICES / MAX_LIGHTS_PER_CLUSTER / CLUSTER_STATS=1` / `MYVK_FRUSTUM_CULL=0` /
  `MYVK_LOD / LOD_BIAS / LOD_FORCE / LOD_DBG=1`。
- **阴影**：`MYVK_SHADOW_DBG=1`(打 `SHADOW-DBG`/`SHADOW-STATS`) / `MYVK_SHADOW=0` /
  `SHADOW_CASCADES=1..4` / `SHADOW_DIST=米` / `SHADOW_LAMBDA=0..1` / `SHADOW_RES=像素` /
  `SHADOW_SNAP=0` / `SHADOW_BLEND=0..0.5` / `SHADOW_HARDEN=0..1` /
  `MYVK_LOCAL_SHADOW=0`(与 `LOCAL_SHADOW_LIGHTS=0` 逐像素等价) / `LOCAL_SHADOW_LIGHTS=n` /
  `LOCAL_SHADOW_RES=像素` / `LOCAL_SHADOW_BIAS=a,b,c`(米/米/纹素，默认 `0.030,0.150,1.2`)。
- **环境/IBL**：`MYVK_SKY / IBL / SKY_ZENITH / SKY_GROUND / SKY_RES / ENV_DBG=1`。
- **Froxel 体积雾（R3-2）**：`MYVK_VFOG=1` / `VFOG_DENSITY` / `VFOG_ALBEDO` / `VFOG_FALLOFF` /
  `VFOG_BLOB` / `VFOG_PHASE_G` / `VFOG_DBG=1`(打 `VFOG-DBG`)。
- **光源修饰（R3-3，只对射灯生效，默认全关）**：`MYVK_IES=1` / `COOKIE=1` / `AREA_LIGHT=1`；
  `MYVK_IES_FILE="name:path;name2:path2"` 加载 `.ies`(LM-63)；`MYVK_LIGHTX_DBG=1` 打 `LIGHTX-DBG`。
  内建纹理名：`cookie:grid|rings|bars`、`ies:narrow|wide|batwing`。真正的开关在组件字段；总开关在渲染器。
  ⚠ **`MYVK_FOG*` 是另一条路径**（解析式**高度雾**，只在 pbr.frag 里按世界坐标求值）——两条独立。
  froxel 雾默认关，关时 set0/binding16 绑 1×1×1 透明图 = 基线逐位不变。
- **R4-1 对照**：`MYVK_CUTOFF_DBG=1` 打 `CUTOFF-DBG mat=.. cutoff=.. lane=..`（每材质节流一行）。
- **R4-2（皮肤 SSS）**：`MYVK_SSS=f` 覆盖曲率缩放（默认 8）。**`=0` → 曲率恒 0 → LUT 退化成 Lambert**，
  这是"关掉 SSS"的对照组。真实开关是材质的 `shadingModel` 字段（Default/Skin）。
  ⚠ **`f ≥ 24` 后曲率被 clamp 到 1 而饱和**（24/32/48/64 逐位相同）→ 做 A/B 对照要选敏感区间内的刻度。
- **R4-3（POM）**：`MYVK_POM=0` 全局关（等价"所有材质 `heightScale=0`"，一步 `return uv` 短路）。
  真实开关是材质的 `heightMap` 槽 + `heightScale`（关键字 bit5 `PARALLAX`）；`heightScale=0` 是
  逐材质的对照组。全局刻度走 `ubo.pomParams`（x/y 淡出区间、z 层数上限、w 倍率兼开关），
  逐材质深度走 `push.skin.y`。
- **R4-4（延迟贴花 DBuffer）**：`MYVK_DECALS=1` 打开（**默认关**；关着时一个 pass 都不录、
  binding 20/21/22 指向 1×1 全透明图）。真实开关是组件的 `enabled` 字段 + `opacity`；
  全局开关在 `ubo.decalParams.x`。诊断：`MYVK_DECAL_DBG=1`（贴花数超 `kMaxDecals=64` 时打 WARN）。
  逐帧打 `DECALS: on/off (dbuffer x3 RGBA8, max 64 decals/slot)`；`CULL-STATS` 尾部 ` decal=%u`；
  `GPU-TIMING` 中部有 `decals=%.2f` 段。
- **R5-1（反射探针）**：`MYVK_PROBES=1` 打开（**默认关**）；`MYVK_PROBE_INTENSITY=f` /
  `PROBE_SIZE=N`（捕获分辨率，默认 **128**，夹 8..1024）/ `PROBE_AF=N`（`autoCaptureFrames`，**0 = 关**，默认关）/
  `PROBE_MAX=N`（默认 4）/ `PROBE_PREFILTER_RES=N`（默认 64，上夹 512）/
  `PROBE_PREFILTER_MIPS=N`（默认 5，**夹紧到 `1+log2(res)`**，见坑点）/
  `PROBE_SAMPLES=N`（预过滤采样数，默认 256）。真实开关是组件的 `enabled` 字段；
  全局开关在 `ubo.probeParams`（offset 752）。
  诊断：`MYVK_PROBE_DBG=1` 打 `PROBE` / `PROBE-RES` / `PROBE-CAPTURE` / `PROBE-FACE` /
  `PROBE-DECIDE` / **`PROBE-DUMP`（逐面加权平均亮度）/ `PROBE-NZ`（逐面非黑纹素计数）/
  `PROBE-MID`（逐面中心纹素实测色）** —— **三个指标交叉验证才可信**。
  ⚠ **探针依赖 IBL 总开关**：`MYVK_PROBES=1` 但 `MYVK_IBL=0` → 静默失效（见坑点表）。
- **R6-1（GPU 粒子）**：`MYVK_FX=1` 打开（**默认关**；关着时**一个字节显存都不分配**
  —— `ensureResources()` 推迟到第一次真的启用）。真实开关是组件的 `enabled` 字段。
  `MYVK_FX_CAPACITY=N`（**每发射器**槽数，默认 4096）/ `MYVK_FX_EMITTERS=N`（最多几个发射器）/
  `MYVK_FX_HZ=N`（模拟固定步长频率，默认 60，上夹 480）/ `MYVK_FX_SIZE=f`（尺寸倍率）/
  `MYVK_FX_SOFTNESS=f`（软化距离倍率；**0 = 关软化，只留硬遮挡**）/
  `MYVK_FX_INTENSITY=f`（亮度倍率）/ `MYVK_FX_FREEZE=N`（模拟钉在第 N 步之后）/
  `MYVK_FX_DBG=N`（**1 = 每 30 帧一条 `FX-DUMP`；2 = 再打一份 `FX-EMITTER`**）。
  前五个之一（`enabled`）接进 `FeatureRegistry`：`fx.enabled` / `fx.sizeScale` /
  `fx.softness` / `fx.intensity` / `fx.freezeFrame`（Engine 面板 + env + 序列化四路派生）。
  诊断家族：`FX: created (enabled=…)` / `FX-DUMP` / **`FX-EMITTER`** / `FX-FREEZE` /
  `FX-PREWARM` / `FX-POOL`（池耗尽警告）/ `FX: … emitters dropped`；
  `GPU-TIMING` 有 `particles=%.2f` 段。
  ⚠ `MYVK_FX_DBG` 没设 → **`FX-DUMP` 一行都不打**（挂在 `m_particleDbg` 闸门后面）。
  ⚠ **`FX-EMITTER` 是唯一能精确验证"旧格式与新格式等价"的手段**（像素做不到，见坑点表）；
  它用 `%.9g`（float32 十进制往返精度）打印，**不能用 `%g`**。
- **UI 自动化**：`MYVK_LOG_RECTS=1`（打印关键控件矩形）。
  粒子面板额外的矩形锚点：`INS-FX-HEADER` / `INS-FX-BURSTS` / `INS-FX-VEL-X` /
  `INS-FX-SPEED-MOD` / `INS-FX-COLOR` / `INS-FX-ALPHA-CURVE` / `INS-FX-SIZE-CURVE` /
  `INS-FX-BLEND` / `INS-FX-SOFT` / `INS-FX-EXPAND-ALL` / `INS-FX-COLLAPSE-ALL` /
  `INS-FX-RAMP-EDIT`。
  ⚠ 折叠/展开的注入变量有**两种解析**：`MYVK_EDITOR_INSP_COLLAPSE`（按空白切分）与
  `MYVK_EDITOR_INSP_EXPAND`（**保留空格**，因为要传 `"Particle Emitter"` 这种带空格的键）。

---

# 已知坑点全表（血泪清单 · 2026-10-14 从 MEMORY.md 搬入）
> 阴影/描述符/a2c/CMake 生成式 `-D` 等已在正文或本归档展开的条目不再重复。

### 构建 / 环境
1. PowerShell 里 `-DXXX=3.5` 会被拆开 → **必须加引号**（含空格的路径同理）。
2. `Window.h` 里 **vulkan.h 必须在 glfw3.h 之前**（`glfwCreateWindowSurface` 被 `#if VK_VERSION_1_0` 包）。
3. 本环境 PowerShell 输出不回显、bash coreutils 不可靠 → 结果写文件再 Read/Grep。
4. mingw 的 `-Wformat` 不认 `%zu`；MinGW 的 `os_defines.h` **已定义 `NOMINMAX`**。
5. **~~改了 GLSL 后 `make build` 不一定把 .spv 拷到 `build/bin/shaders/`~~ 已于 2026-10-14 修复**
   （改挂 `add_custom_target(... ALL)`，见上文《构建：资源拷贝"陈旧副本"缺陷》）。
   **但教训保留**：怀疑任何"改了没反应"之前，先 `cmp` 比一下 `build/bin/` 里的副本，
   或写一句常量红确认代码真的在跑。**别在没排除构建陈旧之前去怀疑算法。**
6. **`GLOB mismatch!` 是 `CONFIGURE_DEPENDS` 的正常提示**，不是错误。

### C++ / 类型
7. `enum class` 底层类型必须是整型；嵌套类型当类型名用要写全限定名。
8. **头文件前向声明的类型，定义不能放匿名命名空间**。
9. `std::filesystem::path` **不能**直接赋 `std::string` → 用 `.generic_string()`。
10. **同一文件的多处修改必须串行下发**（并行 Edit 第二个会静默回滚第一个）。不同文件可并行。
11. `World::clear()` 归零 generation → 旧句柄可能"碰巧" alive；`Scene::clear()` 必须重置 `m_lightEntity`。

### Vulkan
12. 每张交换链图像需**独立** render-finished 信号量；析构时 Swapchain 先于 Surface 销毁。
13. **深度图当纹理采样**用 `DEPTH_STENCIL_READ_ONLY_OPTIMAL`（非 SHADER_READ_ONLY）。
14. **改某条管线的 push constant range = 可能打断整个 Pass 的 set 0 绑定**（VUID-08600）。
15. **DescriptorAllocator 的 0 计数类型不能进 poolSize**（VUID-00302）。
16. **ping-pong 屏障的 oldLayout 要用 UNDEFINED**；**全局描述符引用的图重建后必须 `writeGlobalDescriptors`**。
17. **blit 链生成 mip 时初始 barrier 必须全层 UNDEFINED→TRANSFER_DST**；流式纹理未传层也要转 SHADER_READ。

### GPU 时戳 / 查询
18. **被 reset 却没写入的 query 会永远 unavailable，拖累整批读回**。
19. **fence 初始是 signaled 的，头几帧读 query 违反 VUID-09401**。
20. **`timestampPeriod`/位数是队列族属性不是设备属性**。

### ImGui / 编辑器自动化
21. `ImGui::BeginDragDropSource()` 无 ID 的 item 必须传 `SourceAllowNullID`（否则崩，退程码 `0xc0000409`）。
22. **`ImGui::Dummy()` 天然就是拖放落点**（ItemAdd 传的 ID 是 0）。
23. **ImGui 每帧把聚焦窗口提到最前** → 视口上的 HUD/覆盖层**必须是 viewport 的 child window**。
24. **ImGui 的 popup 必须"每帧都被提交"**；**`BeginMenu` 只有子菜单展开才返回 true**（logRect 写内部）。
25. **`ImGui::EndGroup()` 会 `ItemAdd`**（带自动 ID），`BeginDragDropTarget()` 在其后仍可用。
26. `ImGui::SameLine(offset)` 的 offset 是**相对窗口内容起点**。
27. **固定像素尺寸的浮层会被字号缩放裁掉** → 用 `CalcTextSize` 等字体度量算尺寸。
28. **视口工具栏已经很长**：加按钮会被挤出右边缘裁掉 → 先想"能不能挪到浮层/主工具栏"。
29. `discreteEdit` 的契约是"`*target` 已持有新值，第 4 参 = 旧值"，它只压撤销栈、不赋值。
30. **序列化 `applyCamera` 的 setter 顺序：target 必须最后设**（fly 模式下）。
31. **`logRect` 是按矩形去抖的 → 弹出物的 tag 必须带"第几次打开"的序号**。
32. imgui master(≥1.93) InitInfo 变了；无 docking 分支 → `ImGuiWindowFlags_NoDocking` 未定义。
33. **自动化跑必须把 ImGui 的 `imgui.ini` 一起关掉**（`io.IniFilename = nullptr`）。
34. **自动化启动别读窗口几何存档**；**进程 stdout 是块缓冲**（只读前 24 行 ≠ 卡死）。
35. **自动化 `SetForegroundWindow` 会静默失败** → 加 attach-thread-input 兜底。
36. **自动化注入扩展键（Delete/方向键等）用 `keybd_event` 必须同时给扫描码**。
37. **"注入的第一次左键点击会被 Windows 拿去做窗口激活"**，ImGui 收不到。
38. **注入 Shift+A 的唯一稳法**：ALT-trick 前台确认 + 真实扫描码。
39. **验证层静默是一等验收项**：`grep -c "VUID\|ERROR"` 必须为 0。
40. **临时诊断变量要清零/清日志文件**（多次跑残留会让断言读到旧数据）。

### 颜色 / 贴图 / 其它
41. **数据贴图必须 UNORM**（normal/ORM `srgb=false`，颜色贴图才 true）。
42. **ImGui 颜色经 SRGB 交换链后会明显变亮**（`ImVec4(0.62,0.24,0.22)` 屏幕上约是 0.34 亮度）。
43. **同一张图在不同色彩空间必须是两份 GPU 纹理 → 缓存键要编进 sRGB**。
44. **"因子 × 贴图"的默认值必须是数学中性值**（roughness/metallic 白；emissive 黑）。
45. **`createDefaultTextures()` 的白色(ORM)与平坦法线必须 `srgb=false`**。
46. **环境的钩子初始化顺序**：`MYVK_EDITOR_CAM` 必须放在 `MYVK_EDITOR_SCENE` **之后**。
47. **"可被选中/可被变换"要显式声明**（用标记组件，别靠约定）。
48. **`MYVK_EDITOR_CAM` 的 pitch 是俯角（正=向下）**，setTarget 轨道重摆后要重设。
49. **改项目根目录名会被"以它为 cwd 的进程"挡住**（`mv` 报 busy）→ 先关进程。
50. **`ContentBrowser` 会扫描进程 CWD 的所有图片做缩略图** → 别在项目根目录乱丢验证截图。
51. **自动控制器不要给用户给的"目标值"做无理由的下限夹紧**。
52. **动态分辨率必须把"输出分辨率"与"场景渲染分辨率"彻底分开，且两处一致**。
53. **"关闭 = 基线逐像素不变"在着色器里也要落实到位**（用 0 判断而不是宏）。
54. **bindless 纹理数组容量必须按设备收窄，不能硬写**；材质页表必须**按帧槽分离**。
55. **`#ifdef` 判不出"默认定义为 0 的编译开关"** → 用 `#if` 或 `constexpr`。
56. **REPEAT 采样器 + 0/1 端点是陷阱**：`u=1` / `v=0` 落在纹理边界上时，双线性会与"绕到另一侧"
    的纹素各取一半 → 查表类贴图（预积分 LUT / BRDF LUT / 色彩分级 LUT）的坐标**必须映射到纹素中心**
    `(x*(N-1)+0.5)/N`。R4-2 因它让所有平坦表面无端泛红（差 5.61 vs 修好后 0.20）。
57. **新写的断言必须做"否定测试"**（故意把实现退化回 bug 版本，断言必须变红）。不做这一步的断言
    很可能是空的 —— R4-2 正是靠它把"5.6143"从谜团变成有意的回归守卫。
58. **新增资源目录必须先确认它是不是"编辑器运行时也会写"的目录**：是 → 保持 POST_BUILD（别每次构建
    覆盖用户的编辑）；否 → 挂 `ALL` 自定义目标保证每次构建同步。
59. **高度场的空间尺度必须 ≫ 视差位移**（R4-3）：高度图格宽若与位移同量级，射线走一两步就撞到
    相邻凹格（`h=0`）而提前 `break`，位移被"格宽"截断、**不再随入射角增长** —— 实测 8 格密棋盘
    （格宽 0.125 UV）只测出 1.2× 的角度差异。换成 **2 格**大棋盘（格宽 0.5 UV）才恢复 ∝ tanθ。
60. **淡出 / LOD 类效果的验证机位，必须让"整个可见表面"都越过阈值**（R4-3 D1）：掠射机位
    `y=-1.7` 看似"距墙 10 m"，其实**正前方 1.7 m 就撞墙** → 全屏根本淡不掉，被误读成"淡出没接"。
    换成正视机位（法向距离 9.4 m > 淡出终点 8 m）后逐像素一致。**判失败前先核算几何。**
61. **测"位移/视差方向"的镜像机位，入射角要 ≤ 45°**（R4-3 C2）：位移沿表面的 u 轴，投影到屏幕
    水平的比例是 `r·u = cosθ`。θ=70° 时左右镜像机位的水平分量**同号（都是 +0.34）** → 方向
    完全正确的实现被 `best_hshift` 误判成"方向反了"。θ=45° 时两侧位移才真正反号（实测 ±21 px）。
62. **直接在整张截图上算互相关 = 假阴性**（R4-3 C2）：截图左右两侧是 ImGui 面板（实测 1258 px 宽
    里只有 ~625 px 是视口，x<300 与 x>925 逐列差异恒为 0）。一大片"零位移"会把互相关**硬拉回 0 点**
    → 位移恒测成 0 px。**先把图按 `VP-RECT` 裁到视口再算**（`diff_stats` 内部一直是这么干的，
    但手写的互相关别忘）。
63. **测"均匀位移"要用"处处等高"的高度图**（R4-3 C2）：棋盘高度图下只有凸台有位移、凹槽位移
    恒 0（起点 `h=0` 立即 `break`），列剖面的互相关被两半**平均掉** → 同样测不出。全白高度图
    （1 格）让每像素位移一致，才测得出方向。
64. **推常量 128B 的最后一个空闲位**：`push.skin` 的 `x=jointBase`（VS）/`z=蒙皮标志`（VS）/
    `w=alpha 阈值`（R4-1）/ **`y` 一直没人读** → R4-3 的逐材质视差深度走 `skin.y`。
    硬件把 push constant 卡在 **128 字节**，**这是最后一个能塞逐材质标量的地方**；再要就只能
    进 SSBO / bindless 页表（R4-3 的 heightMap 索引就是复用 `GpuMaterialPage.tex1.z` 的空闲位）。
65. **抓图空图（`GetClientRect` = 0×0）必须走 `capture_nonempty`**：窗口刚 `ShowWindow` /
    被别的进程抢前台时，`capture_client()` 会返回 0×0，`Image.save()` 直接抛
    `ValueError: cannot write empty image` —— 这**不是判 FAIL，是把整个验证脚本崩掉**
    （rc 非 0 且没有任何断言输出）。R4-3 那一轮同时打中 `verify_nav_gizmo` 与
    `verify_light_modifiers`，两者的日志尾部都是同一个 traceback。
66. **"展开折叠段再读日志"的断言必须做成幂等 + 重试**：面板段默认折叠（如 Inspector 的
    `Mesh`），脚本先点标题栏展开、再读里面打的调试日志。**注入的点击偶尔被吞**（第一次左键
    被 Windows 拿去激活窗口 / 此刻布局还在变），于是读到"没有 MESH-PARAMS 日志"的**假失败**
    —— 而且失败项每次还略有不同（典型的 flaky 指纹）。修法：先查日志判断"当前是不是已经
    展开"，未展开才点；点完校验 1 秒，没出现就再点（≤3 次）。
67. ⚠⚠ **深度附件里存的是 NDC z 本身，不是 `(ndc+1)/2`**（R4-4，本轮最贵的坑）。
    本引擎投影由 `glm::perspective` 生成且**没有** `GLM_FORCE_DEPTH_ZERO_TO_ONE`
    → GLM 的 NDC z ∈ [-1, 1]；而 Vulkan 的深度范围**恒为 [0, 1]**
    （`VkViewport::minDepth=0 / maxDepth=1` → `z_f = ndc_z`）。两者叠加 → 深度附件里存的就是
    `ndc_z`。后果：**照抄 `space.glsl::vennViewPosFromDepth` 的 `depth*2-1` 是二次映射** ——
    重建点被拉近 ~1.7 倍（地面点飞到场地上空 2~3 m），盒内测试全数失败 → 一张贴花都看不见
    （`diff on vs base: >18=0`，而日志一切正常：`DECALS: on`、`CULL-STATS decal=1`）。
    **跨 pass 复用几何重建代码之前，先实测确认深度附件的数值约定。**
68. **"窄带身份测试"是核实数值约定的最快手段**（R4-4，一击定位 67）：不做"把调试量编码成颜色
    再读数"（那条路被色调映射堵死了，见 69），而是拿**已知几何**算出"候选约定下各自唯一可能的
    区间"，再看实测落在哪一个。本次：地面 9..16.5 m、`near=0.1 far=200` → 若存 `(ndc+1)/2`
    应落在 `[0.9908, 0.9942]`，若存 `ndc_z` 应落在 `[0.9817, 0.9883]`；探针读三档
    `d≈0.9817 / 0.9850 / 0.9883` → 定案。
69. **把调试量编码成颜色再读数，在带色调映射的管线里是无效方法**（R4-4）：调试值写 `coverage=1`
    后，前向用 albedo × ~2.3 曝光再 `x/(x+1)` → 输出**饱和**，任何编码都被压平（读数恒贴上界）。
    正确做法二选一：① 用**饱和原色做分类编码**（黑/蓝/黄/品红/绿），只回答"走了哪条分支"；
    ② **直接看截图**。想读数值 → 走 68 的窄带身份测试。
70. **顶点属性的 numeric type 必须与 `VkVertexInputAttributeDescription::format` 一致**（R4-4）：
    `assets::Vertex` 的 joints 是 `R8G8B8A8_UINT` → GLSL 必须写
    `layout(location = 3) in uvec4 inJoints;`（**写 `ivec4` 触发
    `VUID-VkGraphicsPipelineCreateInfo-Input-08733`**，管线创建失败、画面**完全没有任何效果**，
    但 `CULL-STATS` 照样报"提交了 1 个" → 排查方向被彻底带偏）。
71. **`re.Match.group(-1)` 抛 `IndexError`**（R4-4）：Python ≥3.6 负数下标**不再**表示"最后一组"。
    共用工具 `last_match(pat, lines, group=...)` 的默认值若写 `-1`，会在**第一次调用**就崩掉
    —— 而且是在前面几组断言都过了之后（R4-4 崩在 B1，A 组 6 项全绿），白烧一轮排查。
72. ⚠⚠ **UBO 是"记录期写、执行期读"的内存 —— 同一份 UBO 在同一个提交里给多个视角/目标复用，
    必然全部用最后一份的值**（R5-1，本轮 head-line bug）。一次 `vkQueueSubmit` 录 6 个立方体面、
    共用同一个相机 UBO 槽 → GPU 执行第 0 面时那块内存里已经是**第 5 面**的矩阵 → 6 面全按第 5 面
    渲染；而绘制列表是**记录期**按各面相机剔除的 → 只有最后一面自洽。
    表现 = **"只有最后一个面有内容"**，极易误诊成"逐面拷贝没生效"或"面序反了"。
    修法 = **每面独占一个 UBO 槽**（`kProbeFaceUboSlotBase = 64`）**+ 一个目标/探针一次独立提交**
    （`rhi::CommandPool` 的 cmd 缓冲不能录两次/提交两次）。离屏目标（`FrameResources::OffscreenUbo`）
    踩过同一个坑 —— **这是第二次，以后写"多视角/多目标"的 pass 先想这一条**。
73. **排查"拷贝 vs 渲染"要用切分实验**（R5-1，两条错误假设都被它一刀切掉）：让 6 面**全用同一个面
    的相机**（临时 `MYVK_PROBE_SAMEFACE=1`）—— 若"六面都有内容"就证明**拷贝链路没问题、问题在渲染**。
    同时排除掉的两条：① "cube-compatible 图像拷贝必须 6 层一组"（**WebSearch 证伪**，逐层 region
    是标准做法）；② "缺内存屏障"（加全量 `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` 屏障**结果零变化**）。
    **临时诊断钩子用完必须清**（`grep -c "TEMP-DEBUG\|SAMEFACE"` = 0）。
74. ⚠ **mip 链长必须与分辨率自洽**（R5-1）：N×N 的图最多 `1+log2(N)` 级（8×8 只支持 4 级）。
    超了是**规范违规**（`VUID-VkImageCreateInfo-mipLevels`），图像创建失败、**整条捕获链作废**
    —— 表现是"开了效果但什么都没发生"，**不崩**。入口已把 `prefilterMips` 夹紧到 `1+log2(size)`。
75. ⚠ **探针（及一切"替换 IBL 镜面项"的效果）的混合在 IBL 总开关内部**（R5-1）：
    `pbr.frag` 的 `if (ubo.envParams.x > 0.5)` 是总闸 —— **`MYVK_PROBES=1, MYVK_IBL=0` 会静默失效**。
    语义上说得通（"关掉环境光"就包含探针），但对使用者是坑 → **必须写进 UI hint**。
    另：**`ambientScale` 会把探针一起乘没** → 验收场地必须 `ambientScale = 1.0`。
76. ⚠ **回读自检要看三个互相不能替代的指标**（R5-1）：逐面**加权平均亮度**（"大面积暗 + 一小块亮"
    会被压扁，亮块占 1/64 时均值只剩 0.3）/ 逐面**中心纹素**（单点说不清整面）/
    逐面**非黑纹素计数**（最不容易自欺，直接回答"这面到底有没有内容"）。
    "均值 0 但中心有颜色"这类自相矛盾的数据，靠三者交叉才能立刻定型。
77. ⚠ **`verify_*.py` 的结果行写法不是自由格式**（R6-1 **第三次**踩）：`run_regression.py` 的 `OK_RE`
    只认 `RESULT: PASS` / `全部断言 PASS` / `^全部 PASS\b` / `结果: PASS`（**ASCII 冒号**），
    取**最后一条**匹配。写成 `PASS —— ...` 或 `PASS: 全部通过` 会**进程 rc=0 但 runner 判 FAIL**
    （日志里连一个 "FAIL" 字样都没有，极易误判成 runner 的锅）。
    **规则写在文档里显然不够** → 新脚本第一件事是**照抄一个已通过脚本的结尾块**。
78. ⚠⚠ **push constant 的「范围」必须在 `VkPipelineLayoutCreateInfo` 里逐字节声明**（R6-1 头号 bug）：
    粒子的**计数**管线只用来写间接命令，看起来"不需要 push 常量"，于是没声明 ——
    后果不是报个错：① 验证层每帧刷 `VUID-VkComputePipelineCreateInfo-layout-07987`；
    ② `vkCmdPushConstants` 的落点**未定义**；③ 最要命的是 `cfg.y`（总槽数）取到 **0**
    → `count = min(counters, 0) = 0` → 间接命令 `instanceCount` **恒为 0** →
    **一颗粒子都不画**，而日志里除了那几行 VUID 什么都没说。
    **范围 = 着色器里 `layout(push_constant)` 那个块，逐字节一致；每个 compute 管线都要检查。**
79. ⚠⚠ **返回类型推导的 lambda：任何一个 `return 值;` 都会把返回类型定死**（R6-1 §14.2）：
    要"提前退出"必须用**裸 `return;`**（或显式写 `-> void`）。把 `return false;` 落在
    末尾无 return 的路径上是 **UB**，MSVC 在 **Debug** 下会在函数末尾塞一条 **trap 指令**
    → 整进程 `0xC000001D`（Illegal instruction）、**一行日志都不给**、瞬间死。
    定位手法：**二分**（逐个模块开关 → 收窄到"只要展开组件就崩"）→ **读回生成的代码**。
    另一个化身：`return false;` 与 `ImGui::PopID()` 的**顺序被写反**（重写时把签名里的
    `-> bool` 去掉、却漏了那个 `return false;`）。
80. ⚠ **冻结帧（`MYVK_FX_FREEZE=N`）不是"逐像素一致"**（R6-1 实测，口径改了两次才写对）：
    它保证的是**状态**逐位可复现（`alive/draws/cap/maxAlive/simFrame/inst` 在所有配置的所有次
    运行里**完全相同**），**像素**做不到。两条固有不确实性：
    ① **池按 frames-in-flight 分槽**，每槽只在自己被派发时推进（`dtSlot = elapsed * dt`），
       冻结后各槽停在**各自的最后一步**（差 1~2 步）→ 恰好落在边界上的那颗粒子生/死不同
       → 一整块粒子面积（≈250 px）变。**所以差异不随速度放大**（10 倍初速反而更小）——
       **差的是"集合"不是"位移"**；**一颗粒子也有 0.0082 的差**、且软粒子开关对它读数**完全相同**。
    ② **紧凑表顺序来自 `atomicAdd` 的完成顺序**，加色混合数学上无序，但帧缓冲是 **8 位 UNORM**：
       每个片元的 `+=` 都要量化一次 → 差值随**重叠的部分覆盖率像素**增长
       （364 颗羽流：软粒子开 1.59~2.13、关 0.058，**27 倍**）。
    ⇒ **凡有"原子顺序 / 多槽分帧 / 8 位量化"的地方，"逐像素一致"这条断言就不要提**；
    **先实测再定阈值**，不要猜。
81. ⚠ **验证"序列化新旧格式等价"不要用像素**（R6-1）：改用 `MYVK_FX_DBG=2` 的 `FX-EMITTER`
    （把交给 GPU 的发射器数据逐值打出）→ 等价性退化成**一次字符串比较**，精确、零噪声，
    且顺手让配置可以脱离编辑器审计。**必须 `%.9g`**（float32 十进制往返精度）；
    `%g` 默认 6 位**会把不同的值打成一样**，"相等"的结论就没意义了。
82. ⚠ **搜索/过滤的「兜底条件」必须与「逐项放行条件」同源**（R6-1 的真实现 bug）：
    粒子面板逐模块放行按"模块名 **+** 模块关键词"，兜底却只按"模块名"
    → 搜只在关键词表里的词（`billboard`）得 **12 个模块全显示**，过滤等于没生效。
    测试期望值同样栽在这上面：拿 `"particle"` 当"组件级关键词"来测兜底，
    可它是 "Soft **Particle**s" 这个**模块名**的子串 ⇒ 过滤到只剩 soft 才是对的。
83. ⚠ **`FX-PARAMS` 的 `modules=` 只登记「画出来的、且展开的」模块**（R6-1）：
    一个都没有时写的是**字面量 `(none)`** 而不是空串（解析必须归一化）。
    ⇒ 测"搜索过滤"必须**先把 12 个模块全展开**再搜，否则被命中的模块默认折叠 → 永远是 `(none)`
    （"实现是对的、期望是错的"的典型）。
84. ⚠ **注入"展开某个分区"可能是空操作**（R6-1）：`MYVK_EDITOR_INSP_EXPAND="Particle
    Emitter,fx/playback"` 里 `fx/playback` 本来就默认展开 → 注入等于没注，
    而 `fx/*` 的**模块**展开状态由 ImGui 自己存，`SetNextItemOpen(cond=Appearing)` 对
    **已存在**的节点不再生效 → **"一键全展开"必须用 `ImGuiCond_Always` 强推一帧**。
85. ⚠ **抓图偶发偏一点不要靠放宽阈值解决**（R6-1）：同一场景连续抓 9 次全 `0.000000`，
    整套脚本跑十几次里出现过一次 `0.261`（窗口抢占 / 捕获落在尺寸未落定的一帧）。
    处置 = **失败时重抓 N 次取最小差**，**判据一个字不放宽**。放宽门槛 = 把断言变成永远绿。
