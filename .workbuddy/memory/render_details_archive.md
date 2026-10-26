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

### Inspector 左侧图标导航条（E11.1 实现细节 · 2026-10-16）

叙事（动机 / 图标缺陷发现过程 / 假红 / 否定测试）→ `2026-10-16.md`。这里是**实现契约**。

**文件分工**
- `editor/InspectorIcons.h`（新）：`IconPen` 小笔 + 11 个图标函数。
  `IconPen` 提供 `at/len/line/rect/rectFill/circle/disc/ellipse/arc/triFill/quadFill`，
  入参是**归一化 0..1 的格内坐标**，换算成屏幕坐标。
  ⚠ **刻意不提供 `AddPolyline` 的包装** —— 它的 `(thickness, flags)` 参数在新版 ImGui
  调换过，包一层等于把签名歧义藏起来。
  **颜色由调用方给**（UI 图标 = 控件）；`EditorIcons.h` 那批颜色写死（资产图标 = 内容）——
  这是两个文件的分工契约。
- `editor/InspectorPanel.h/.cpp`：页签枚举 / 表 / 派发 / 落盘 id 桥。
- `editor/EditorLayoutIO.h/.cpp`：`LayoutState.inspTab`（**字符串 id**）。
- `editor/EditorApp.cpp`：`tabIndexById()` 应用 + `tabId()` 回写。

**页签枚举与表**
```cpp
enum class InspTab : int { Object=0, Tool, Render, Output, Scene, World,
                           Particles, Physics, Material, Animation, All, Count };
struct TabDesc { const char* id; const char* label; const char* tip;
                 void (*icon)(ImDrawList*, ImVec2, float, ImU32); };
const TabDesc kTabs[] = { {"object","Object","变换 / 可见性 / 网格几何",iconObject}, ... };
static_assert(kTabCount == static_cast<int>(InspectorPanel::InspTab::Count), "...");
static_assert(static_cast<int>(InspectorPanel::InspTab::All) == kTabCount - 1, "...");
```
**枚举整数值 = 图标栏自上而下的顺序**；`tabId(i)` / `tabIndexById(s)` 是唯一桥，
**落盘写 id 不写下标**（中间插页签不会读歪用户的布局）。

**导航条度量**（全部从字体派生，不写死像素）
- `btn = ImGui::GetFrameHeight()`（本机 28px）、`colW = btn + WindowPadding.x*2`。
- `BeginChild("##insp_nav", {colW,0}, Borders, NoScrollbar)` +
  `PushStyleColor(ChildBg, (0.09,0.095,0.105,1))`。
- 每项：`InvisibleButton("##tab", {btn,btn})` + **自绘**圆角底（`Header`/`HeaderHovered`）
  + 选中项左侧 2px 竖指示条；图标 `inset = btn*0.165f`。
- `All` 前插 `Dummy(4) + Separator() + Dummy(2)` 做语义分隔（验收脚本会断言这个空隙）。
- 每项 `logRect("INS-NAV <id>")`。

**内容区**：`SameLine()` + `BeginChild("##insp_content")` + `drawTabContent(e)` +
`drawNoMatchHint()` + `EndChild()`。

**页签初始化三级优先**（`draw()` 内一次性）
1. `MYVK_EDITOR_INSP_TAB=<id|下标>`；
2. 否则 `editorLayoutAutomationRun()` → `All`（**兼容层**，见下）；
3. 否则 `Object`。

⚠ **`All` 不是"融合视图"，是旧的堆叠绘制顺序一字不改**（`drawTransform / drawVisibility /
drawMeshAndMaterial / drawLights / drawScript / drawAddRemove`）。存在的唯一理由是
**9 个旧验证脚本靠 `INS-*` 锚点断言分区存在**，自动化下默认它才能让它们继续绿。
**别"优化"它的顺序。**

**搜索优先于页签**：`m_search[0] != '\0'` → 忽略页签选择、走 `drawTabAll(e)`
（并提示 "(searching across all tabs)"）。

**两处容易搞错的细节**
- `bulletWrapped(text)` = `Bullet()` + `SameLine()` + `TextWrapped()`：
  **`BulletText()` 不换行**，长句会被右边缘直接裁掉。
- `drawSceneryFallback()` 拆成 `drawCameraSection()` / `drawSunSection()` /
  `drawSceneStatsSection()` 三段，供 `world` / `scene` 页签复用。

**锚点语义（本轮最值钱的一条）**
`INS-MESH-HEADER` / `INS-COLLISION-HEADER` / `INS-PROBE-HEADER` 记在 `sectionHeader()`
**之后**、`if (!open) return;` **之前**（为了让脚本能点到折叠的分区标题把它展开）。
⇒ 它们**只能证明"函数被调用了"**，不能证明"分区可见"。
想断言"分区可见"必须用记在提前返回**之后**的锚点：`INS-MATERIAL-HEADER` /
`INS-OUTPUT-HEADER` / `INS-ANIM-HEADER` / `INS-ADD-COMPONENT` / `INS-SCENE-STATS` /
`INS-CAMERA-HEADER` / `INS-SUN-HEADER`。**已有一条断言因为搞混这条而假红过。**

**落盘**：`LayoutState.inspTab`（默认 `"object"`）→ JSON 键 `inspTab`；
`LAYOUT-LOAD` 日志带 `inspTab=%s`。写盘守卫沿用全编辑器统一规则（自动化下拒写）。

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

---

# 规则全文（2026-10-16 从 `MEMORY.md` 迁入）

> **为什么迁**：`MEMORY.md` 被注入时会**截断**（实测只进得去开头约 12KB / 全量的 ~20%），
> 于是文件后半的规则**从来没有到达过模型** —— 2026-10-16 就是因为快照里没有「回归执行纪律」
> 而误跑了一次全量回归。
> **现在 `MEMORY.md` = 纪律 + 速查 + 指针（目标 ≤ 14KB，能整份进注入窗口）**；
> **这里 = 规则全文**。改动前除了读 `MEMORY.md`，**必须按下表读本文件对应小节**。

## 目录（从 MEMORY.md 迁入的部分）
- 特性注册表（E01） · 材质（Alpha / 着色模型 / POM） · 变体与关键字 · 延迟贴花（R4-4）
- 反射探针（R5-1） · GPU 粒子（R6-1） · 渲染管线细则（阴影 / 分簇 / FrameGraph / FrameUBO / set0）
- 布局范式（E04~E08） · 核心面板（E09~E17） · 交互理念（E18/E19/E21）
- 断言纪律（完整） · 编辑器验证工具链（完整） · 高频坑点（19 条）
- 环境变量补充（E04~E08）

---

## 特性注册表（E01）

`engine/render/FeatureRegistry.{h,cpp}` = 可调项的**唯一事实源**。现状 **28 组 / 103 参数 / 0 未接线**。

- **加新参数 = 两步**：① 在 `kFeatures[]` 对应 `kParams*[]` 加一行 `ParamDesc`；
  ② 在 `Renderer::bindFeatureParams()`（或 `bindPostFeatureParams()`）加一行
  `bindParam("id", &成员, kind)`。
  漏② → 启动时 `FEATURES-UNBOUND` WARN 点名；漏① → 参数根本没地方写。
- **两条不能破的约束**：
  ① 权威存储仍是原来那 4 个设置结构体 + Renderer 成员，注册表**只有地址**；
  ② `bindParam` 必须在**成员默认初始化之后、`loadEnvOverrides()` 之前** —— 绑定那一刻的值会被
  记成默认值（供 `resetToDefaults()`），顺序反了 reset 就失效。
- **`loadEnvOverrides()` 可多次调用**，只处理"尚未应用过"的绑定（`applied` 标记）→ 构造函数 +
  `createMainTarget()` 各调一次是**设计如此，不是 bug**；也正因如此，`init()` 里被设备能力修正过的
  开关（如 `m_bindlessEnabled` 被强制关）不会被第二次重读打回去。
- **`EnvMode` 11 种必须逐条对应原解析函数**，不许"顺手统一"（边界行为会变）。最易错的：
  `PFloatClamp`（`atof` 直取，**不拒绝负数** → 夹到下界）vs `PFloatClampNN`（`envFloat` 语义，
  **拒绝负数** → 保持默认）。`MYVK_RENDER_SCALE` 属于后者。
- **不进注册表的**（有理由）：`MYVK_IES_FILE`（字符串清单）、`MYVK_VIGNETTE_OFF`（反向开关）、
  `MYVK_VIEW_CAM`（用哪台相机）、`MYVK_AE_SIM_LUM`（调 setter）、全部 `*_DBG`（诊断闸门）。
- 新参数加完跑 `check_feature_surface.py -v`：H 组打印"注册表接管度"，未接管的（值在函数局部
  变量里）是 **E10** 的输入。
- ⚠ **`FeatureRegistry.*` 在 `GLOBAL_GLOBS` 里** —— 动它就全量重跑（现 **49 例**）。

## 材质（Alpha 模式 + 着色模型 + POM）

- `AlphaMode`：Opaque / **Blend**（不写深度、透明队列降序、**跳深度预通道**）/
  **Mask**（`alpha < cutoff` → `discard`、写深度、走**不透明队列**、**跳深度预通道**）。
  **队列分类唯一判据是关键字位** `hasKeyword(kw, kKeywordAlphaBlend)`。
- **cutoff 载体 = `push.skin.w`**；**符号位 = 抖动开关**（负 → Bayer 4×4）。唯一出口
  `Renderer::alphaCutoffLane(mat)`（非 Mask 恒推 +0.5）。MSAA 下开 a2c + 预映射
  `clamp((alpha-cutoff)*2+0.5)`；⚠ **a2c 只允许 `samples != 1`**（1x 下开会让创建管线报错）。
- **`ShadingModel{Default,Skin}`**：Skin 把漫反射乘子从 `NdotL` 换成预积分 LUT 查表
  `texture(sssLut, (N·L*0.5+0.5, curv))`；**方向光 / `shadeLocalLight` / `shadeLocalLightMod`
  三处都要分支**；曲率 `clamp(length(fwidth(几何法线)) * sssParams.x, 0, 1)` ——
  **必须用扰动之前**的 N；采样用 `textureLod(...,0)`（隐式 LOD 会糊掉响应）。
- ⚠ **查表坐标必须钉到纹素中心** `(x*(N-1)+0.5)/N`：采样器是 **REPEAT**，而端点 `u=1` / `v=0`
  （平面 `fwidth(几何法线)=0` → 曲率恒 0）**正好落在纹理边界** → 双线性掺进"绕到另一侧"的纹素
  → **平坦皮肤无端泛红**（R4-2 实测 0.20 → 5.61）。**这条适用于所有查表类贴图。**
- 序列化：`alphaMode`（字符串，**保留旧 `alphaBlend` 布尔兼容**）+ `alphaCutoff` + `ditherAlpha` +
  `shadingModel` + `heightMap` / `heightScale`。**色彩空间是契约**：albedo/emissive `srgb=true`；
  **normal/ORM/高度图等数据表必须 false**。
- **POM（R4-3）**：`Material::heightMap` + `heightScale`（UV 单位）→ 关键字 **bit5 `PARALLAX`**。
  全部贴图采样改用 `parallaxUv()` 的结果 `matUV`，但 **TBN 必须仍用未位移的 `vUV`**（位移后的 UV
  在淡出/早退的**分支边界**不连续 → `dFdx` 拿到错的切线基）。几何法线要在 POM 里**重算一份**
  （含双面翻转），不能借用随后被法线贴图扰动过的 `N`。`scale <= 0` 第一步就 `return uv` ——
  这就是"heightScale=0 逐像素不变"的依据（全局 `MYVK_POM=0` 同路短路）。
  ⚠ 高度图必须 **`srgb=false` + REPEAT**：sRGB 解码会把 0.5 压成 0.216（起伏形状被扭过）；
  视差一旦生效 UV 必然被推出 [0,1]，CLAMP 会在边界拉出硬缝。

## 变体与关键字

`ShaderKeywords.h` 唯一事实来源；**位序三处逐字对齐**：`kKeywords[]` / `CMakeLists` 的
`SHADER_KEYWORDS` + `VARIANT_DEFINES_ALL` / `pbr.frag` 的 `#if`。

bit0 `DOUBLE_SIDED` / bit1 `ALPHA_BLEND` / bit2 `NORMAL_MAP` / bit3 `ALPHA_MASK` /
**bit4 `SHADING_SKIN`** / **bit5 `PARALLAX`** → **64 变体**（**变体 ID = 掩码本身**）。

- 变体表由 CMake 循环生成（**不手写**）。⚠ 空串会被 `list(APPEND)` **丢弃 → 位序整体错位**：
  空项存哨兵 `"_"`、`list(JOIN DEFS " " DEFS_STR)` 拍平、末尾
  `list(TRANSFORM ... REPLACE "^_$" "")`；每套必须 `separate_arguments(DEFS NATIVE_COMMAND ...)`
  再喂 glslc。
- 硬约定：绑定缓存哨兵**不能是 0**（0 是合法掩码）→ `kBindsNothing = 0xFFFFFFFFu`，栅格后重置；
  `SHADER-VARIANTS` 日志必须打在前向 Pass **之后**；变体**共用同一 `VkPipelineLayout`**。
- `#extension` 必须在所有非预处理语句之前。全局开关（bindless / instancing / MDI）走独立 .spv。

## 渲染管线细则（阴影 / 分簇 / FrameGraph / FrameUBO / set0）

Pass 序：`Shadow(4 级级联) → LocalShadow(12 层数组) → DepthPrePass → SSAO(compute) →
ClusterBuild(compute) → VolumetricFog(compute×2) → Forward(天空盒→不透明→栅格→透明) →
PostProcess(Bloom+Tonemap) → 交换链`。

- **compute 不能在 RP 内执行** → 深度预通道独立成 RP 且**无条件执行**（只把"画几何"包开关），
  否则前向 RP 的 `LOAD` 深度未定义。
- **栅格必须夹在不透明与透明之间**（吃深度测试 + 不写深度），且**绝不能**做成 ImGui 叠加层
  （ImGui 永远压 3D 之上）。**Forward 内部子顺序刻意如此**：栅格排在那里才既被墙挡、又能被玻璃叠。
- **阴影（方向光 CSM）最易忘三条**：① 矩阵必须 **Zero-to-One**（`glm::orthoRH_ZO`，不是
  `glm::ortho`），且 `pbr.frag::shadowFactor` **只对 xy** 做 `*0.5+0.5`、z 直接用；
  ② CSM 每级范围 = **该级视锥切片的包围球**（不是物体 AABB），球心在**光轴基**量化到纹素网格
  —— ⚠ **不能量 `view*mid`**（`mid` 是 lookAt target，`view*mid ≡ (0,0,-dist)`，量化 = 空操作）；
  正交**近平面取 0**、靠 `kDepthPadding = 25m` 外扩，退化时 `radius` 下限 0.05；
  ③ 只接收不投射的物体必须 `VisibilityComponent::castShadow=false`（自带地面就是）。
  级联分割 PSSM practical λ=0.6，**near/far 必须从 `camera().nearPlane()/farPlane()` 读**
  （级联分割、切片角点 NDC 换算、分簇对数切片三处假设同一组值）；级联盒**边缘不淡出**
  （只在"视深度 > 最大距离"处 `smoothstep`），否则交界处假暗带。
- **阴影（局部光）最易忘三条**：① 近平面按 range 取 `clamp(range/20, 0.1, 2.0)` —— 透视 NDC
  深度是**双曲**的，range=32 用 near=0.05 时"离灯 15m 遮挡体"与"20m 地面"NDC 只差 7.5e-4
  → 阴影整个消失；② 深度偏移以「米」给、片元按参考深度换算回 NDC
  （`LightShadowRecord.params=(near,far,灯下标,有效位)`；`pbr.frag::ndcBiasFromWorld`：
  `dz/dd = (f − z·(f−n))² / (n·f·(f−n))`）—— **别给双曲深度配 NDC 常数偏移**（等于 10 米世界深度）；
  ③ **未使用的层每帧也要走一次空 depth-only pass（只清深度）**：整张 array 被采样器一起绑定，
  留一层停 UNDEFINED → 验证层报 IMAGE_LAYOUT。层分配：按到相机距离平方由近到远贪心，
  预算不够就整体停在那一盏之前（点光 6 层/聚光 1 层，`kMaxLocalShadowLayers=12`）。
- **阴影（共用）**：`shadow.vert` 靠 `ShadowPushConstants.which.y` 选矩阵来源
  （0→`FrameUBO.lightSpace[x]`，1→set0/binding11）—— **矩阵只有一份**；
  `ShadowPass` 推常量是 96B 的 `ShadowPushConstants`（非主前向的 128B），因为它在**自己的 RP 内
  自行绑 set 0** → **绝不要把该 Pass 绘制搬进前向 RP**（VUID-08600）。
  **array 深度图渲染**：framebuffer/`imageStore` 不能挂 array 视图做单层渲染（那是 layered
  rendering）→ 每层单独建 `VIEW_TYPE_2D` 视图 + framebuffer；采样侧才用 `sampler2DArrayShadow`。
- **分簇**：簇编址 C++ / `cluster_build.comp` / `pbr.frag` **三处逐字一致**；`maxLightsPerCluster`
  唯一来源是 C++（`ubo.lightParams.w`），**禁硬编码**；固定槽位 → 结果可复现。
- **FrameGraph**：新效果登记依赖、**关时不得录 pass**；读 MSAA 深度要另编 `*.msaa.*` 并进 CMake
  列表；`recreateScreenSpace()` 早于 `postInputViews()`。
- ⚠ **`FrameUBO` = 768B**（720 → 736 `pomParams` → 752 `decalParams` → **768 `probeParams`**，
  末尾 offset **752**；`RenderTypes.h` 有 `static_assert(sizeof(FrameUBO) == 768)`）。改它必须同步
  **所有**声明它的着色器（`pbr.vert/frag`、`sky.vert`、`cluster_build.comp`、`shadow.vert`、
  `fog_inject.comp`、**`decal.vert/frag`**）；新字段**只能加末尾**。
- ⚠ 推常量 **128B 全满**：`mat4 model + vec4 baseColor + vec4 pbr + vec4 emissive(w=材质页号) +
  vec4 skin(x=关节基, y/z=蒙皮标记, **w=alpha cutoff**（符号位=抖动开关）)`；
  **`skin.y` = 逐材质 POM 深度**。**旁路管线的 push range 必须与主管线逐字段相同**
  （Vulkan 把 range 与阶段算进"布局兼容"）→ 否则 VUID-08600，整个 Pass 的 set0 一起挂。
- **set0 = 25 binding**（0 FrameUBO / 1 shadowMap / 2 lights / 3 clusters / 4 lightIndices /
  5 clusterStats / 6 jointMatrices / 7 irradianceMap / 8 prefilterMap / 9 brdfLut / 10 skyMap /
  11 lightShadows / 12 localShadowMap / 13 instanceModels / 14 ssaoTex / 15 mdiInstances /
  16 vfogTex / 17 lightExt / 18 lightTex / **19 sssLut** / **20-22 DBuffer 三张** /
  **23 probeMap(`samplerCubeArray`)** / **24 ProbeBuffer**，全表见本文件上面的 set0 表）。
  新增要同步 `kGlobalSetBindings` + `writeGlobalSet` + 着色器 layout **三处**。
  ⚠ DBuffer 缺省槽必须绑 **`m_defaultTransparent`**（RGBA 0,0,0,0）—— **不能**用
  `m_defaultBlack`（alpha=255 → 整屏被当成 coverage=1 的黑色贴花）。
  ⚠ 探针 binding 23 缺省槽必须绑 1×1 全黑 cube array（6 层）兜底图，**不能"不绑"**：
  layout 有该资源 → 任何用 `pbr.frag` 的管线都必须有有效描述符，否则
  `VUID-VkGraphicsPipelineCreateInfo-layout-07988`（与 SSAO 缺省白图同辙）。
- **材质贴图 set = 7 个**（set1 albedo … set6 normal / **set7 heightMap**）。Vulkan 只保证 4 个
  绑定集，本引擎实际要 **8 个**（RTX 3060 报 `maxBoundDescriptorSets=32`，AMD/Intel/Adreno/Mali
  普遍报 8，正好卡线上）→ **再加一张贴图槽就得改架构**（并进 set0 数组或改 bindless 专用）。
- 全 SRGB，**着色器内不做手动 gamma**；`addFace(n,u,v,half)` 中 **u×v 必须 = n**。

## 延迟贴花（R4-4 DBuffer）

- 一条贴花 = 一个**盒体**（渲染器自建单位立方体，**画正面 / cull back**）。片元**不采盒体表面**，
  而是从深度图 `texelFetch` 重建场景表面世界坐标 P → `worldToLocal` → `|local|/halfSize` 最大值
  `m > 1` 就 `discard`。**这条盒内测试同时替代了深度测试** → pass 里**没有深度附件**
  （场景深度是 MSAA 4x，DBuffer 单采样，采样数不同不能共用同一 RP 的深度附件）。
- ⚠⚠ **深度附件里存的是 NDC z 本身，不是 `(ndc+1)/2`**：投影是 `glm::perspective` 且**没有**
  `GLM_FORCE_DEPTH_ZERO_TO_ONE` → NDC z ∈ [-1,1]；Vulkan 深度范围**恒为 [0,1]** → `z_f = ndc_z`。
  所以重建**不能**照抄 `space.glsl::vennViewPosFromDepth` 的 `depth*2-1`（那是二次映射 → 重建点被
  拉近 ~1.7 倍 → 贴花全不可见）。`worldPosFromDepth` 直接用 `vec4(uv*2-1, depth, 1.0)`。
- ⚠ `worldToLocal` = **`glm::inverse(base)`（不含 size 缩放）**，**不是** `inverse(it.world)` ——
  否则 `local` 被反成单位立方体坐标，而盒内测试与 uv 投影（`local.xy/size.xy`）都按**米**写。
- 法线局部→世界用 **`transpose(mat3(rec.worldToLocal))`**（刚体逆的转置 = 旋转本身），
  **不能**用 `mat3(rec.world)`（含非均匀 size 缩放 → 法线扭歪）。
- ⚠ **两处 `dFdx/dFdy`（重建法线的 `dFdx(P)`、贴图 uv 的 `dFdx(duv)`）必须在所有 `discard`
  之前无条件算完** → 贴图采样走 `textureGrad`，**不是** `texture()`。被丢弃的辅助调用导数是垃圾。
- `DecalRecord`（std430 **192B**：world/worldToLocal/tintOpacity/sizeEdge/params/fade）走 **SSBO**
  而非 push（两个 mat4 = 128B 就吃满 push 下限）；push 只剩 4B 下标。
  `DecalBuffer` 自有 set0 = `{FrameUBO, 深度图, 记录 SSBO}`，set1 = 逐贴花 `{albedo, normal}`。
- DBuffer = 3 × `R8G8B8A8_UNORM`（**必须线性**，法线/粗糙度做 sRGB 解码会整体扳偏）。
  混合语义（`PipelineDesc` 只有一套）：`rgb = src·src.a + dst·(1-src.a)`、
  `alpha = src + dst·(1-src.a)` → 清屏 `alpha=0` ⇒ 前向跳过整段（这就是"关 = 基线逐像素不变"的依据）。
- 全局开关走 **`ubo.decalParams.x`**（**不是**关键字位 —— 贴花开关可随时切，关键字位留给材质属性）。
  `MYVK_DECALS` 默认**关**；`kMaxDecals = 64`/帧，超出丢弃并 WARN。
  GPU 计时 `kSectionCount` 9 → **10**（新 `kDecals`），`CULL-STATS` 尾部 ` decal=%u`。
- ⚠ **析构必须真的销毁资源，不能只 `vector.clear()`**：一个 `DecalFrame` 带 image + view + memory
  + framebuffer 四个**裸句柄**，`clear()` 只析构元素、不调 `vkDestroy*` → 3 张图 × 2 帧 =
  **20 个对象泄漏**。`DecalBuffer` 是**无条件构造**的（与 `MYVK_DECALS` 无关），所以泄漏每次运行
  都在 `vkDestroyDevice()` 被点名。**凡是持有裸 Vk* 句柄的帧结构，析构都要走 `destroyFrames()`**
  （对照 `VolumetricFog` / `SSAOPass` 的写法）。

## 反射探针（R5-1）

逐项实现 + 21 项验收 + 坑点全表 → `2026-10-15.md` §12。

- ⚠⚠ **UBO 是"记录期写、执行期读"的内存 —— 同一份 UBO 在同一个提交里给多个视角/目标复用，
  必然全部用最后一份的值。** 头号 bug：6 个立方体面共用一个相机 UBO 槽 → 6 面全按第 5 面的相机
  渲染；而绘制列表是**记录期**按各面相机剔除的 → 只有最后一面自洽，表现成**"只有最后一个面有
  内容"**（极易误诊成拷贝/面序问题）。**修法 = 每面独占一个 UBO 槽**
  （`Renderer::kProbeFaceUboSlotBase = 64`，base+0..5）**+ 一个探针一次独立提交**。
  离屏目标（`FrameResources::OffscreenUbo`）踩过同一个坑 —— **这是第二次**。
  **排查手法**：切分实验（让 6 面全用同一面的相机 → 若"六面都有内容"就证明问题在渲染不在拷贝）。
  已证伪的两条假设：① "cube-compatible 图像拷贝必须 6 层一组"（逐层 region 是标准做法）；
  ② "缺内存屏障"（加全量 `ALL_COMMANDS` 屏障结果**零变化**）。
- ⚠ **一个探针 = 一次独立提交**：`rhi::CommandPool` 的 cmd 缓冲**不能录两次/提交两次**，
  多探针必须各取各的 `beginOneTime()`。
- ⚠ **mip 链长必须与分辨率自洽**：N×N 的图最多 `1+log2(N)` 级（8×8 只支持 4 级）。超了是
  **规范违规**，图像创建失败、**整条捕获链作废**（表现"开了探针但什么都没发生"，不崩）。
  入口已把 `prefilterMips` 夹紧到 `1+log2(prefilterSize)`。
- ⚠ **探针混合在 IBL 总开关内部**（`pbr.frag` 的 `if (ubo.envParams.x > 0.5)`）：探针替换的就是
  IBL 的镜面预过滤项。**`MYVK_PROBES=1, MYVK_IBL=0` 会静默失效**（已在面板 hint 写明，
  **没有**把混合移出 IBL 分支 —— 有意识的取舍）。
- ⚠ **`ambientScale` 会把探针一起乘没**（`ambient = (diff+spec) * ao * ambientScale * ssao`）
  → 验收场地必须 `ambientScale = 1.0`。
- ⚠ **GLSL 里 `struct Probe` 必须定义在 interface block 之外**（结构体不能定义在块里）。
- ⚠ **探针分区默认折叠**（`sectionHeader(..., defaultOpen=false, ...)`）→ 参数行在
  `if (!open) return;` **之后**，headless 点不到 → 断言只能做"节标题在不在"（`INS-PROBE-HEADER`）。
- 回读自检报**三个交叉验证指标**才可信：`PROBE-DUMP`（逐面加权平均亮度，会被"大面积暗+一小块亮"
  压扁）/ `PROBE-MID`（逐面中心纹素，单点说不清整面）/
  **`PROBE-NZ`（逐面非黑计数，最不容易自欺）**。
- **探针判定的三条或关系**：① 显式请求；② 数据变了 或 `captures == 0`（首帧必烘）；
  ③ `autoCaptureFrames > 0` 且过期。**`autoCaptureFrames` 默认 0（关）** —— 默认"变了才烘"。
- **组件**：`ecs::ReflectionProbeComponent`（`extents` 盒体半轴 / `intensity` / `boxProjection` /
  `blendDistance` / `enabled`），位置 = 实体 Transform；序列化字段 `probe`；
  ⚠ `SceneSerializer` 的存活条件是 `mesh|mat|decal|probe` **四者取或** —— 漏一个探针实体就被整个
  吃掉。

## GPU 粒子系统（R6-1）

逐项实现 + 34 项验收 + 实测表 → `2026-10-15.md` §13~§18。

- ⚠⚠ **描述符池按"类型"给容量时，要数 set layout 的**全部 binding**，不是"实际写了几条"**
  （2026-10-16 修，P0）：`vkAllocateDescriptorSets` 那一刻就把**整个 layout** 的每种描述符从池里
  扣掉。`createDescriptors()` 原写 `maxStorageBuffers = framesInFlight * 8`（注释还把计数/绘制各
  算成 2 个），而实际每帧是 **14** 个 `STORAGE_BUFFER`（模拟 4 + 计数 6 + 绘制 4）→ 预算 8/帧
  < 需求 14/帧 → **第二帧一开始就 `VK_ERROR_OUT_OF_POOL_MEMORY`（-1000069000）**，
  表现是 `MYVK_FX=1` **一开就** `Fatal: Vulkan call failed`（**空场景也一样，与 UI/场景无关**）。
  已改 `* 16`（留余量）。**加/删任何 set 的 binding 都要回来核对这一行。**
- ⚠ **`进程存活=False` / `client=(0,0)` 不等于"进程被杀了"** —— 它可能**早就 `Fatal` 退出了**。
  排查这类"编辑器莫名提前退出"时，**第一件事是打印子进程的最后 N 行**（本轮靠这个一击命中
  `[VK_CHECK] Vulkan error -1000069000`）。别一上来就套"抓图 flaky / kill_stale_editors 误杀"。
- ⚠ **脚本崩溃会把后面的断言整组遮住**：`verify_particles` 的 A4 无条件拼 `err_a2 + err_a3`，
  而它们只在"A2 抓图成功"分支里赋值 → 抓图失败时抛 `UnboundLocalError`，
  **脚本带 traceback 死掉、走不到结尾的 `结果:` 行** → 回归里只剩 rc=1 + Python 堆栈。
  B~G 七组因此**从未跑到**，修完池之后才第一次露面（3 项红）。
  **凡"先决条件失败就要跳过后续"的脚本，都要保证那条路径是"判 FAIL + 报清楚"，不能是崩溃。**
- ⚠⚠ **push constant 的"范围"必须与着色器里的 `layout(push_constant)` 块逐字节一致**，
  **每个** compute 管线都要写（`ParticleSystem::createPipelines` 里那条计数管线漏了）：
  `VkPipelineLayoutCreateInfo` 里不声明 → 验证层刷 `VUID-...-layout-07987`，
  `vkCmdPushConstants` 落点**未定义**，且 `cfg.y`（总槽数）取到 **0** → `instanceCount = 0`
  → **一颗粒子都不画**，日志里只有那几行 VUID。
- ⚠⚠ **返回类型推导的 lambda：任何一个 `return 值;` 都会把返回类型定死**，要提前退出必须用
  **裸 `return;`**。混进 `return false;` 而末尾无 return 是 **UB**，MSVC Debug 会在末尾塞 trap
  指令 → 整进程 `0xC000001D`，**一行日志都不给**。（本轮 `InspectorPanel` 的 `module()` 这么死的；
  另一个化身见 `2026-10-15.md` §14.2。）
- ⚠ **冻结帧（`MYVK_FX_FREEZE=N`）保证的是「状态」可复现，不是「像素」逐像素一致；
  而「状态」本身也**分两种**：**真确定**的字段（`cap`/`emitters`/`frozen`/
  `instAlpha`）跨运行**精确**相同，**与"冻结那一刻截到哪一槽"有关**的字段
  （`alive`/`draws`/`instAdd`/`maxAlive`/`simFrame`）只能**有界**。
  两条固有不确实性：① **池按 frames-in-flight 分槽**，每槽只在自己被派发时推进，冻结后各槽停在
  **各自的最后一步**（差 1~2 步）→ 边界处那颗粒子生/死不同 → 一条**集合**差异（所以**不随速度
  放大**，10 倍初速反而更小）；**另一条同源的 ±1 在 `simFrame` 上**：累加器允许一帧补多步
  （`kMaxSimStepsPerFrame=8`）→ 越过 FREEZE 的那一帧可能从 119 直接跳到 121；② **紧凑表顺序来自
  `atomicAdd` 完成顺序**，加色混合数学上无序但
  **8 位 UNORM** 下每片元 `+=` 都要量化 → 差值随**重叠的部分覆盖率像素**增长（软粒子开 1.59~2.13、
  关 0.058，27 倍）。⇒ 断言口径（**2026-10-16 修正**）：**确定字段精确 + 跨槽字段差有界
  （`alive`/`draws`/`instAdd` ≤8、`maxAlive` ≤2、`simFrame` ∈ [N, N+7]）+ 像素差有界（≤4.0）**；
  "冻结后真的停住"另用**同一趟内多个样本互等**来精确验（`verify_particles` 的 B3b）。
  ⚠ 旧口径写的是"状态逐位一致（精确）"—— **与 ① 自相矛盾**，照它写的 B1/B3 会随机红
  （实测 5 次：`alive` 极差 4、`simFrame` 有 1 次是 121）。
  ⚠ 引擎里 `FX-FREEZE` 那句日志（"state is now reproduced bit-for-bit across runs"）
  **是过度承诺**，`Renderer.cpp` 冻结分支 —— 改它要动 `engine/**`（欠一次全量），暂留。
- ⚠ **验证"序列化新旧等价"不要用像素** —— 用 `MYVK_FX_DBG=2` 的 **`FX-EMITTER`**（把交给 GPU 的
  发射器数据用 `%.9g` 逐值打出）。等价性退化成**字符串比较**。
  ⚠ 必须 `%.9g`（float32 十进制往返精度）；`%g` 默认 6 位会把**不同的值打成一样**。
- ⚠ **12 个模块的折叠键必须带 `fx/` 前缀**（`m_sectionOpen` 是全局表，`INSP-SECTIONS` 日志会把它
  打出来 → 裸名 `playback`/`shape` 会污染 `verify_inspector` 的断言）。
- ⚠ **一键展开/折叠必须 `ImGuiCond_Always` 强推一帧**：`SetNextItemOpen(Appearing)` 对**已存在**的
  ImGui 节点不再生效，光改 `m_sectionOpen` 是点不动的。
- ⚠ **`CollapsingHeader` 的 ID 取 `"##"` 之后的部分** —— 12 个模块不能都叫 `"X##fx"`。
- ⚠ **`FX-PARAMS` 的 `modules=` 只登记「画出来的、且展开的」模块**；一个都没有时写**字面量
  `(none)`** 而不是空串（验证脚本必须归一化）。所以测"搜索过滤"必须**先把 12 个模块全展开**再搜。
- ⚠ **搜索的兜底判据必须与逐项放行判据同源**（见「断言纪律」同名条）。
- **组件是纯 POD**（`colorRamp.stops[8]` / `bursts[4]` 全是数值，无 `std::string`/`vector`）
  —— 这是"整块进撤销栈"与 Copy/Paste Properties（`PK_ParticleEmitter`）的前提。
- **序列化两头都写**：`bursts` 数组 + `burst = bursts[0].count`；`colorRamp` 数组 +
  `colorStart/Mid/End = ramp.eval(0/0.5/1)`；读则反之回落。
- **色带撤销记整条带子**（拖完会 `sort()` 重排，按单个停靠点记会让撤销写回**别的**点）。
- **`Add stop` 插在最大空隙中间并取该处 `eval()` 颜色** ⇒ "加点从不改变外观"。

## 布局范式（E04~E08）

清单与逐条记账 → `docs/editor_roadmap.md` §3。

- **面板表 = `EditorApp` 的静态成员 `panelTableVec()`**（不是自由函数 + `friend`：那要求两处
  声明逐字一致，漂移编译期查不出来）。加面板 = `buildPanelTable()` 加一行 + 一个静态薄壳
  `panelDrawXxx(EditorApp&)`（表项要访问私有成员，薄壳里 `a.m_xxx->draw()`）。
- **`PanelVisibility` 不是存储，是「命名引用视图」**：权威存储在 `PanelRegistry.cpp` 的
  `bool g_open[kMaxPanels]`。**必须是固定数组，不能用 vector**（持一堆 `bool&`，扩容就悬垂）；
  **必须是引用不是每帧拷贝**（`MenuItem(..., &p.stats)` 直接写指针，副本改了不回写）。
- **面板快捷键一律 `Alt+1..8`**。`Ctrl+1/3/7` 与 `Ctrl+2/4/6/8` 被 `handleViewShortcuts()` 的
  视图方向键占；工作区用 `Ctrl+Alt+1..6`。三条顺序硬约束：`Ctrl+Alt` 分支排
  `handleViewShortcuts()` **之前**；按住 Alt 时不调它；`Shift+Space`（最大化）排
  `Space`（播放/暂停）**之前**且 Space 分支排除 `io.KeyShift`。
- **`autoRatio` 逐节点**（不是一个全局 `m_userAdjusted` —— 那样拖根节点会冻住右区的自适应）。
  **面板关掉时那一侧收成 0**，且不画那条没意义的分隔条。
- **折叠 `collapsed` 与 `ratio` 的耦合**：折叠靠 `ratio = 0/1` 实现，而 `0` 又是"自适应"哨兵 →
  落盘时折叠中的节点必须**写真实比例**、读盘时走独立分支（`setRatio` 顺带 `autoRatio = false`）。
  否则折叠一读回来就丢（而 `collapsed` 还是 true，自相矛盾）。
- **`m_maximized` 只有一份权威状态**（在 `SplitLayout`），其余三个面板写 0×0 +
  `EditorApp::drawPanels()` 用 `showsInLayout(slot)` 跳。`computeLayout()` 只在
  `L.viewport.w/h > 1` 时才从视口底部扣播放条（否则最大化 Hierarchy 后底部凭空冒一条）。
- **多视口 = 挂在 `m_targets` 尾部的 RenderTarget**。**必须进 `m_targets`**：相机 UBO 槽按
  `m_targets` 下标分配，同一命令缓冲录多台相机每台要自己的槽。`SubviewSlot::target` 是
  **非拥有裸指针**（写 `unique_ptr` = 双拥有 = 运行时双重释放）。缩容 `erase` 后必须
  `reindexTargets()`。`setSubviewCount(0)` 是默认值 → 单视口下逐像素不变。
- **`forceShaderReloadScan()` 不直接 poll**：把 `m_shaderReloadTick` 顶到 15 让**下一帧**越过
  节流；直接 `poll()` 会把"变了"消费掉 → `drawFrame` 那轮看不到 → 管线永不重建。
- ⚠ **`TR-BAR` 那一行只能是灰阶**：`verify_add_menu.py` 断言 2 取该行"高饱和"
  （`max-min > 45`）像素的质心、断言落在水平正中。任何彩色控件都会把它拽偏 → **误报 FAIL**。
  Transport 加的时间轴区块全部 `PushStyleColor` 成灰阶，且只吃左侧（`leftBudget = centerX - 24`）。
- ⚠ **两条分隔条会叠在同一像素**（某节点折到 0/1 时压在父节点线上）→ 必须有"悬停独占"
  （子树先于父节点，先到先得），否则一次双击"折一条 + 展一条"同时发生。
- **多视口四路**：`MYVK_EDITOR_VP_LAYOUT=<single|2v|2h|quad>`；随工作区落盘。
  ⚠ **E07 后延四项仍未做**（子视口性能降级 / 独立视图模式与 gizmo / 点击激活 / 视口矩形数组）
  —— 已落地的是"能同时出多个视角"那一半。

## 核心面板（E09~E17）

一句话目标：**"每个面板都能搜、能过滤、能拖、能落盘，且每项都有自己的 `verify_*.py` 且 PASS"**。
逐项实现 → `docs/editor_roadmap.md`「阶段 C 落地记录」；过程坑点全表 → `2026-10-15.md`。

- **新面板 = `PanelRegistry` 加一行**（View 菜单 / 快捷键 / 图标 / 落盘四路自动派生）；
  面板之间**只准依赖 `EditorContext`**，要动引擎就走 `EditorHost` 纯虚接口。
- **Inspector**：分区折叠状态按**分区名**存（`m_sectionOpen`，换实体不重置）；
  Copy/Paste/Reset 是 **POD 字节快照**（`sectionPodOf` 返回 `{ptr,size,def}`），
  写入必须经 `RawBytesEditCommand`（可撤销）。`insp.selftest` 是机器可验证的自检命令。
- **Hierarchy**：过滤要**保留祖先链**（`subtreeMatches` 递归查后代，未命中的祖先灰显）；
  增删实体**不在遍历 ECS 的过程中做** —— 走 pending 队列，树画完 `flushPending` 统一经
  `structuralEdit`（带撤销快照）；
  ⚠ **状态列的 `InvisibleButton` 必须画在右键菜单之后**（`BeginPopupContextItem` 绑的是"最后
  一个 item"，抢在它前面会让右键菜单失效）；拖放落点 `Dummy` 的**宽度必须铺满**
  （写 0 = 无可命中矩形，真鼠标拖不到）。
- **ContentBrowser**：面板默认只有约 290px 宽 —— 工具栏必须**单行**，面包屑走**三级降级**
  （全显示 → `...` + 末尾若干段 → 只留当前目录名）；`ThumbnailCache` 是 LRU，
  **GPU 上传必须在渲染线程**（真正的后台解码另外记账）。
- **E14 视口浮层**：必须是**子窗口**（独立顶层窗口会被焦点窗口盖住 —— 视口每帧把焦点窗口
  `BringWindowToDisplayFront`）；两层显隐 = 总闸 `m_showOverlay`（`MYVK_EDITOR_NO_HUD` 的落点）
  × 四块各自开关（`EditorContext::ViewportOverlay`，右键空白处 `Viewport Display` 菜单 +
  偏好落盘）。**总闸一关四块全灭 —— 这是 NO_HUD 逐像素回归的前提。**
  ⚠ **四角布局 = 左上模式条 / 右上统计 / 左下相机信息 / 右下导航球**（导航球从**左下搬到了
  右下**）。浮层既然是子窗口，**就会先吃掉落在自己身上的点击** → "点视口某角落当空地"的用例
  全废了（`verify_bvh` 就栽在这：它点右下角 94%,94% 当空地，E14 之后那里是导航球 →
  `miss 日志 0 -> 0`）。**凡是靠"点视口空白处"做断言的用例，一律以 `MYVK_EDITOR_NO_HUD=1` 启动。**
  ⚠ 浮层子窗口高度必须用 `GetTextLineHeightWithSpacing()` / `GetFrameHeightWithSpacing()`
  （不带 Spacing 的版本少算几像素 → 冒滚动条、最后两行被滚出可视区）；
  子窗口一律加 `NoScrollbar | NoScrollWithMouse`。
- **数字键已被两套快捷键占满**（`1/3/7(+Ctrl)` 视图方向、`Alt+数字` 面板显隐）
  → E14 的三件事全挂 Shift 层。⚠ **`Shift+N` 切的是模式索引 `N-1`**：`Shift+2 → unlit`、
  `Shift+5 → metallic`、`Shift+6 → wireframe`。书签跳转**只恢复 yaw/pitch、保留当前注视点**
  （比连位置一起还原好用）。
- ⚠ **`logRect` 不是逐帧刷的**：同 tag 只在矩形变化 >4px 时打一条。
  ① 想断言"某块还在不在画" → 用只在**状态变化**时打的
  `VP-OVERLAY master=.. modeBar=.. stats=.. camera=.. navGizmo=..`；
  ② **不要拿 `VP-RECT` 去裁图**：视口首帧布局用的是"启动那一刻的窗口尺寸"，resize 与首帧的
  先后不同会留下不同首值（同一份二进制实测两种都出现过）。像素比对用**整窗**（含面板边框，
  最强），或视口内部稳定锚点 `VP_INNER=(330,200,1200,750)`。

### 窄栏属性行的槽位宽度（2026-10-16 补 · 物理页 / 粒子页踩到）

属性行有两种写法，**宽度约束完全不同**，别混：

| 写法 | 结构 | 约束 |
| --- | --- | --- |
| `propRow`（`kLabelWidth = 88` 那套） | 标签列 + 值列，`SameLine(colValue)` 绝对定位 | **标签必须 ≤ 12 字符**（88px 装得下 "Normal Scale" ≈ 84px） |
| 裸 `SetNextItemWidth(w)` + `Slider*/Combo/Drag*` | 控件在前、**标签在控件右侧** | 标签宽度 = `面板内容宽 − w − ItemSpacing`，**超了就贴不出省略号、直接被裁** |

面板最小宽 330（`SplitLayout::kRightMin`），扣掉**图标导航条 ~48**、内边距、**竖向滚动条 ~14**
之后，**一行只有 ~250px 可用**：

- 槽位 `140` → 只剩 ~102px 给标签，`Angular Thresh`（≈105px）正好被切最后 1 个字符；
- 槽位 `180`（粒子 FX 段）更狠，`Freeze frame`（≈84px）也被切。
- ⚠ **滚动条是叠在内容之上画的**，所以"某一行是否被切"取决于**该行在面板里的纵向位置** ——
  同一列里 `Velocity Iters` 没被切、`Angular Thresh` 被切，就是这个原因。**首屏截图未必看得到，
  必须看到底部那一屏。**

**约定**：物理页统一 `kPhysParamW = 110.0f`（世界 + 刚体，21 处），粒子 FX 段
`kFxParamW = 140.0f`（原 180）；常量定义在 `InspectorPanel.cpp` 文件头 `namespace {` 里
（**必须在第一个使用点之前** —— 下方 4100 行那个 `kLabelWidth` 常量块够不着）。
新加属性行时**先数一下该列最长的标签**再定宽度。

## 交互理念（E18/E19/E21）

逐项实现 + 两条设计取舍 → `docs/editor_roadmap.md`「阶段 D 落地记录」。**E20 待 E03、E23 待做**。

- **命令注册表（E18）= 编辑器里所有"动作"的 SSOT**：面板表 / 视图模式 / 工作区 / 控制台命令
  **全部从既有注册表派生**（`render::kFeatures[]`、`PanelRegistry`、`ConsolePanel::commands()`…），
  **不许手抄第二份清单**。加一条命令 = `reg.add` 一行。实测 `CMD-REGISTRY count=71`。
- **命令面板开着时 `handleShortcuts()` 必须停**（`if (!m_showCommandPalette)`）—— 否则用户在输入框
  里敲 "s" 会顺手触发吸附、敲数字会切视角。且 `drawCommandPalette(...)` **不能**放进
  `if (m_showCommandPalette)`：env 钩子（`MYVK_EDITOR_CMDPAL`）需要"面板还关着"的那一帧来把它
  打开。
- **E19 参数撤销的钩子必须在 `SetTooltip` 之前取 item 状态**：`SetTooltip` 会提交一个新 item，
  之后 `IsItemActivated/IsItemDeactivatedAfterEdit` 问的就是 tooltip 了。
  `ParamEditCommand::name()` 返回 `const char*` → 名字**预拼**成成员（现拼要用静态缓冲，多条历史
  会互相覆盖）。合并是**三重条件**：同一命令对象 + 同 paramId + 500ms 窗口（外加"栈顶就是它"）。
- **E21 通知中心不复用 Console 的 sink**：`core::Logger` 有两条**正交**通道 —— `setLogSink`
  （显示端，Console 面板独占）与 `setLogHook`（通知端）。混用会让 Console 自己的回显行（`> cmd`）
  也变成 toast，且"有没有 Console"决定"有没有通知"。**只有 `Warn/Error` 自动升级**，`Info` 只能
  显式 `push`（否则启动几百条 Info 糊满屏）；同文本在 toast 存活期内**去重**（`repeat` 计数）。
- **toast / 任务条 / 历史窗口必须最后画**（ImGui 后画的窗口在上层），且游戏全屏提前 return 那条
  路上也要画 —— 全屏跑游戏时恰恰最需要"刚才出错了"。
- **点 toast 的落地在宿主**：`NotificationCenter` 只 `takeConsoleFocusRequest()`；`EditorApp` 负责
  `panels().console = true` + `ConsolePanel::requestScrollToText()`（只记请求，真正的 `SetScrollY`
  发生在它下一次 draw，帧内不做跨面板迭代）。UI 点击与自动化 `notif.click` 走**同一条**
  `clickToast()`。
- **进度取消的契约**：`requestCancel()` 只置位；`setTaskProgress()` 返 **false** 才表示"取消已被
  消费" → **由宿主** `endTask()` 收尾。取消的任务**不发**完成通知。
- **`NOTIF-STATUSBAR` 是边沿触发**：只在（errors / warns / tasks / dirty）变化时打，
  **tool 提示不进判等**（它每帧都变，进来就退化成每帧一条的噪声）。
- 通知历史落盘 `config/editor_notifications.json`，守卫与布局/偏好**同一条规则**
  （自动化下拒写；显式 `MYVK_EDITOR_NOTIFICATIONS_PATH` 时读放行、写仍禁止）。
- ⚠ **整窗像素基线会因状态栏变化而红**：`verify_viewport.py` 的 V3 是"与 E14 改造前的整窗参考图
  一致"。E21 给状态栏加了四个常驻项 → 实测差 4692 px、bbox 只在 y[825,840]（客户端底部
  `kStatusH=26`）。**修法是拆断言、不重采基线**（重采 = 把当前二进制像素盖成"参考"，V3 退化成
  自己跟自己比）：V3 = 状态栏以上整窗 0 差；V3b = 差异全落在那一只条内且非零。
  **以后动状态栏/工具栏那两条带，先想到 V1b/V3 会不会红。**
- ⚠ NO_HUD 会话里**出现 WARN/ERROR 会让右上角多一条 toast**，同样会破坏 V1b/V3 的整窗逐像素
  比对 —— 那说明渲染/脚本真报了警告，**让它先红，别改阈值**。

## 断言纪律（完整）

- ⚠ **采样区必须落在被测物体上**（俯角 8° 时视口**上半是天空盒**，"视口正中"只采到天空 →
  误判"没生效"）。先拿哨兵确认可见性；用**面板区**或"有物体 − 无物体"现算掩膜再腐蚀几像素。
- ⚠ **必须 `MYVK_EDITOR_NO_HUD=1`**（`drawOverlay` 会画 FPS/帧时间）+ `MYVK_GRID=0`。
- 色调映射压平高光 → **别用固定倍数阈值**，用"逐级落差 > ε"或单调性。**"全有/全无"的开关测不出
  "分级"** → 取**敏感区间内**的中间刻度。
- **亚像素效果不能靠截图验证**；**"输入驱动"的行为别只靠像素差分**。
- ⚠ **"状态一致"与"像素一致"是两个断言，别混**：凡有**原子顺序 / 多槽分帧 / 8 位量化**的地方，
  前者可精确断言，后者只能**有界**断言。先**实测**再定阈值，不要猜。
- **新写的断言必须做"否定测试"**（故意把实现退化回 bug 版本 → 断言必须变红）。
  **临时诊断代码用完必须清。**
- **"同一份产物、失败项每次不同" = flaky**，先别怀疑功能，检查断言的**度量口径**：
  ① 是不是拿"整幅图统计量"比较**样本数不同**的两个画面（用差异带/面板区）；
  ② 同一行里两个阈值是否自洽（`n` 统计 `>18` 却说 `mx > 8` 就算显著 → mx=17 时自相矛盾）；
  ③ 是否依赖**注入时序**（点击/焦点）。三类项目各踩过一次。
- ⚠ **FAIL 了先怀疑「期望」再怀疑「实现」**：某轮 5 项 FAIL 里 3 项是**期望写错**
  （注入是空操作 / 没先展开被搜的模块 / 拿模块名的子串当"组件级关键词"），1 项是**新断言的
  口径本身不成立**（要求像素逐位一致），只有 1 项是真代码 bug。
- ⚠ **兜底条件必须与放行条件同源**：逐项放行的判据包含 A、兜底判据只看 B ⇒ 两者必然打架
  （实例：搜索过滤按"模块名+关键词"放行、兜底只按"模块名"→ 搜只在关键词表里的词会全放行）。
- ⚠ **"抓图偶发偏一点"不要靠放宽阈值解决**（见坑点 #85）：**失败时重抓 N 次取最小差**，
  判据一个字不放宽。**放宽门槛 = 把断言变成永远绿。**
- ⚠ **`MYVK_FRAMES=N` 会让进程"几秒跑完"**（无 vsync 时 ~450fps，`MYVK_FRAMES=900` 只活
  2.07 秒）→ "启动 → 等日志 → 截图"拿到的是**已退出的窗口**（`GetClientRect` 返 `0×0`、
  `PrintWindow` 返 `False`、`capture_stable()` 恒 `None`）。
  **需要截图的用例不要用 `MYVK_FRAMES` 控帧**，改成"等条件出现 + 主动关窗"；
  `MYVK_NO_WINDOW_SAVE=1` 已足够让 `automationRun()` 为真（写盘照样禁止）。
- ⚠ **脚本结尾必须打 `结果: PASS` / `结果: FAIL: ...`（ASCII 冒号，行内可被 `OK_RE` 命中）**。
  `run_regression.py` 只认 `RESULT: PASS` / `全部断言 PASS` / `^全部 PASS\b` / `结果: PASS`
  四种，取**最后一条**匹配 → **自由格式（如 `PASS —— ...` / `PASS: 全部通过`）会让 rc=0 但判
  FAIL**。⚠⚠ **这条已踩第三次**（R5-1、R6-1 各一次）→ **新脚本的第一件事就是照抄一个已通过
  脚本的结尾块**，别自己发挥。
- ⚠⚠ **锚点语义：`logRect` 记在"提前返回"之前的锚点只能证明"函数被调用"，不能证明"分区可见"。**
  想做"锚点应当消失"的负向断言，必须用记在返回**之后**的锚点。（E11.1 真踩：`INS-MESH-HEADER`
  为了"能点到折叠标题"刻意记在 `if (!open) return;` 之前 → 搜索把它滤掉了它照样打 → 假红。）

## 编辑器验证工具链

- **`tools/check_feature_surface.py`**：特性↔UI 覆盖自检（**秒级、不跑 GUI、可进 CI**）。
  维护"特性登记表"（`FeatureDesc` 的离线投影），grep `editor/` 校验每个特性的 **UI 入口真的存在**。
  查 A 登记完整 / B 文档 / **C 无 UI 面★** / D 裸 env 漏网 / E 回归保护 / F GPU 段名 / G 陈旧登记。
  **C、D 组必须清零**。⚠ 加白名单 = 显式断言"用户不需要在界面摸到它"，加前三思。
  ⚠ 它**排除自己**再扫仓库，否则清单里的 env 名会让"每个 env 都存在"，G 组永远无法报红。
- **`tools/run_regression.py`**：读末尾 `结果: PASS`；**增量**；**新增 `verify_*.py` 必须加进
  `DEFAULT`**，并且**顺手补一条 `TRIGGERS`**（这是最容易漏的一步 —— 2026-10-16 就漏了）。
  它自己**不参与任何用例的指纹**（它只是"跑法"）。`--explain` = 只打印影响分析不跑，
  **改完先看一眼会跑哪些**。
- **跳过 = 两道闸都放行**：
  ① **用例脚本**：该脚本 + 它递归 `import` 的本地 tools 模块没变（修一个脚本只重跑它自己）；
  ② **变更影响分析**：把构建输入（`WATCH_ROOTS`）拍快照存缓存，下次 diff 出"改过哪些文件"，
  按 `GLOBAL_GLOBS` / `TRIGGERS` / `IGNORE_GLOBS` 三张表映射成**关联用例集**，只有落在这个
  集合里的才重跑。实测：只改 `decal.frag` → 只跑 1 例（30+ 分钟 → ~100 秒）；
  快照与上次完全一致 → **一例都不跑**（"没变 ⇒ 结果不变"，连"没登记"的也不跑）。
- ⚠ **映射表四条铁律**：
  1. **没登记 = 全量 + 点名**（保守兜底），**绝不静默跳过** —— 表可以增量补，补漏不会漏测；
  2. **不要把横切文件写进 `TRIGGERS`**（命中 `GLOBAL_GLOBS` 永远**先**按全量处理，写进去是
     死代码，而且 `--narrow` 下会变成"看着有归属、其实没覆盖"的**假跳过**）；
  3. **判定顺序 = 先横切 → 再忽略 → 最后映射**（`IGNORE_SUFFIXES` 里的 `.txt` 曾把
     `CMakeLists.txt` 当文档吞掉 → 改了构建配置却什么都不跑）；
  4. ⚠ **判断"某文件是不是横切"只能去 `GLOBAL_GLOBS` 里查，别凭印象。**
     `GLOBAL_GLOBS` 里**没有 `editor/**`** —— 曾因此把 `InspectorPanel.*` 误当成横切文件，
     结果 `verify_particles` 的面板断言在"改面板"时被**静默跳过**（2026-10-16 修）。
- **横切文件是"文件粒度"的天花板**：`Renderer.cpp` 里改的是哪一段看不出来 → 只能全跑。
  确认真只动了一个特性时用 **`--narrow`**（作者显式断言其它特性不受影响）跳过这层，
  再把该特性用例**显式点名**（点名的永远跑）。
- ⚠ **不要用 `engine/render/**` 这种整片 glob 当触发**：它会把"新增的、还没人认识的 render
  文件"也判成**已登记**，从而绕过"未登记 → 全量"的兜底。要用 `RENDER_MODULES`（逐个模块列），
  `check_regression_map.py` 的 [F] 项会核对"engine/render 下每个文件要么在 GLOBAL、要么在
  RENDER_MODULES"，漏列会被抓出来。
- **资源泄漏门**：全套里**只有 `verify_collision_play` 会让编辑器优雅退出**，因而只有它的日志里
  会出现 `vkDestroyDevice(): ... leaked objects`（其余脚本都是强杀进程，泄漏报告根本不产生）。
  所以它被挂上**整个渲染层**（`RENDER_MODULES`），回归脚本另有一道**搭车式兜底**：扫本轮跑过的
  每个用例日志，命中泄漏行就报 `== 横切检查 ==` 并把退出码置 1。
  ⚠ 这条门是"只跑关联用例"的**必要配套** —— 跳过的用例不再提供覆盖率，横切不变量必须有专责。
- 快照**不含运行时数据**（`assets/scenes`、`assets/scripts` 是编辑器运行时会写的目录），
  也不含 `tools/`、`docs/`、`.workbuddy/`。
- **`tools/check_regression_map.py`**：**映射表自检**（秒级、不跑 GUI）。检查影响分析的 13 个
  行为分支、快照覆盖范围、映射覆盖率、**死映射**（写了不存在的路径）、**全横切登记**
  （假跳过风险）。改完映射表就跑它。
  ⚠⚠ **改 `TRIGGERS` 时它很可能先红** —— `[A]` 组有几条硬编码期望（如
  `editor/InspectorPanel.cpp` 应命中哪几个用例）。**改 `TRIGGERS` + 同步它的 fixture 是一个
  原子操作**（2026-10-16 就是这么发现漏改的）。它报红时会**把多出来/缺了的用例名打出来**。
- 共用设施 `capture_window.py`（`find_window` / `capture_client` →
  ⚠ **返回 `(PIL.Image, ok)` 元组，不是 ndarray，也不能直接切片**；`user32` /
  `SetWindowPos` / `ShowWindow`）/ `verify_add_menu.py`（`LogTap`：`start(proc)` /
  `last_rect(tag)` 前缀匹配返 `(x0,y0,x1,y1)` / `has_line(needle)` / `.lines` / `.lock`；
  `kill_stale_editors` / `wait_rect`）。
  ⚠ 抓图**一律用 `capture_nonempty`**（已转发到 `capture_stable`），别直接
  `capture_client(...)[0].save(...)`。
- ⚠ **`capture_stable` 挡的两种 flaky**：① **client 0×0**（窗口还没铺开）—— 直接 `save` 会抛
  `ValueError: cannot write empty image` **把整个用例崩掉**（不是判 FAIL，是崩）；
  ② **尺寸正常但整幅纯白**（`PrintWindow(PW_RENDERFULLCONTENT)` 在 Vulkan 交换链上偶发返回未
  合成帧；实测 1280×800 只压出 **4460 字节**）。第二种最阴险：空图 vs 正常图做差 = "差异铺满整窗
  / 采样区全 255"，**看起来像渲染回归**。判空规则：整窗缩到 32×20 后 min/max 灰度差 < 3
  （编辑器整窗恒有面板文字，整窗单色必然不是真画面）。
  ⚠ 凡是自己写 `capture_client` 的用例都要补上这层（`verify_csm` / `verify_ibl` 因此红过一整轮）。
- **Python（本机）**：`C:\Users\wumen\.workbuddy\binaries\python\envs\default\Scripts\python.exe`
  （3.13.14，numpy 2.5.4 + pillow 12.3.0）。⚠ **裸 `python` 不带 PIL/numpy** → 回归会整批 FAIL，
  错误是 `ModuleNotFoundError: No module named 'PIL'`（**看着像代码炸了，其实是环境**）。
  ⚠ 记忆里曾记的 `C:\Users\v_lweiili\AppData\Local\Programs\Python\Python312\python.exe`
  **在本机根本不存在**（那是另一台机器的路径，2026-10-16 核实）—— 照抄会直接失败。跑回归固定写全路径。
- 截图目录 `<repo>/../_venn_verify`；⚠ **禁止硬编码绝对盘符**。
- 离线烘焙：`make_bc_dds.py`（BC 编码器，可复用）、`make_sss_lut.py`（R4-2 LUT）。日志标签表见本文件。

## 高频坑点（19 条 · 全表 85+ 条见上面 `# 已知坑点全表`）

> 其中「串行 Edit / vulkan.h 顺序 / VUID-08600 / DEPTH_STENCIL_READ_ONLY / HUD 子窗口 /
> imgui.ini 与 SetForegroundWindow / cmp 旧副本 / REPEAT 边界 / 键鼠注入组合 / 幂等重试 /
> uvec4 / group(-1) / 深度约定 / 颜色编码 / 抓图 flaky」**已在上面 85 条表里**，此处不重复。
> 下面只留**上面没有**的（2026-10-16 去重后剩余）：

1. **同一文件的多处修改必须串行 Edit**（并行第二个会静默回滚第一个）；不同文件可并行。
2. **凸体要画正面**（cull back）：凸体正面投影 == 整个盒体投影，盒内像素必被覆盖；代价是相机进入
   盒体内部时片元全为背面 → 一个都不产生（已知边界情形）。
3. **递归 lambda 不能按名字自引用** → 把"自己"作为第一个参数传（`[&](auto&& self, ...)`），
   比 `std::function` 少一个 include、少一层类型擦除。
4. **`unique_ptr` 成员与"统一由容器持有"不能并存**：目标若必须挂在 `m_targets` 里（为了让下标
   == 槽位），槽位结构体里就只能是**非拥有裸指针**；写 `unique_ptr` 编译器可能侥幸放过，
   运行期一定双重释放。
5. **C++ 成员/内联实现里写错成员名只在实例化时报**（`subviewCount()` 里的 `m_subviewTargets`
   vs `m_subviews`）—— 报错行指向 `.h`，但根因是名字对不上，先怀疑拼写。
6. **优雅关闭类自动化测试：窗口在 `onInit` 之前就建好了**。"找到窗口"≠"跑了几帧" ——
   立刻发 `WM_CLOSE` 会拿到 `Total frames: 1` 的无意义会话；必须再等几秒。
7. **顶点属性的 numeric type 必须与 `format` 一致**：`Vertex` 的 joints 是 `R8G8B8A8_UINT`
   → GLSL 必须 `uvec4`（**写 `ivec4` 触发 VUID-Input-08733**，管线创建失败、画面全无效果，
   但 `CULL-STATS` 照样报"画了 1 个" → 极具误导）。
8. **调试量编码成颜色再读数不可靠**：输出经 exposure × ~2.3 + tone-map(`x/(x+1)`) 会**饱和**，
   读数恒贴上界。**改用饱和原色做分类编码**（黑/蓝/黄/品红/绿）+ 直接看截图。想核实数值约定
   则用**窄带身份测试**（拿已知几何算出唯一可能的区间，再看落在哪个约定里）。
9. **ImGui 1.93（`IMGUI_VERSION_NUM 19298`）可用/不可用**：`ImGuiChildFlags_Borders`、
   `AddQuadFilled` / `AddTriangleFilled` / `AddEllipse` / `PathArcTo`+`PathStroke` 可用；
   ⚠ **`AddPolyline` 的 `(thickness, flags)` 参数在新版调换过**，**不要包一层**
   （`InspectorIcons.h` 的 `IconPen` 刻意不提供它的包装）。

## 环境变量补充（E04~E08）

- `MYVK_EDITOR_WORKSPACE=<level|rendering|character|animation|cinematic|debug>`（启动切工作区）
- `MYVK_EDITOR_WORKSPACE_SEQ="30:debug 60:level"`（运行中**按帧**切 —— E05 验收要"切过去再切
  回来"，一次会话得切两次）
- `MYVK_EDITOR_VP_LAYOUT=<single|2v|2h|quad>`
- `MYVK_EDITOR_LAYOUT=<布局文件路径>`
  ⚠ **落盘的读/写不对称**：自动化运行下**默认路径不读**（残留布局破坏像素级对照），
  **但显式 `MYVK_EDITOR_LAYOUT` 照读**（脚本自己要求的文件，否则落盘没法自动化验）；
  **写则一律拒绝**（别把测试的布局存成"用户的"）。
- E11.1 新增：`MYVK_EDITOR_INSP_TAB=<id|下标>`（见「Inspector 左侧图标导航条」一节）。

# 路线图与已知未解问题（明细 · 2026-10-16 从 MEMORY.md 迁入）

> MEMORY.md 只留结论与"下一项"；这里的明细在**开工前挑活**时读。

## 渲染遗留

- **R6-1 GPU 粒子（✅ 主体）** 遗留：ribbon / mesh 模式 / **粒子碰撞** / 视口内预览 / 帧图事件 /
  `duration=0` 时 Burst 不发射。
  **基准**：用户两次强调"以 unity 或 blender 为基准"（面板 = Unity Shuriken 12 模块，
  颜色 = Blender Color Ramp 带停靠点色带）。
- **R5-1 反射探针（✅ 主体）** 遗留：SSR↔探针权重混合 / 相邻探针边界混合 / 探针 gizmo · 预览球 ·
  独立烘焙。**R5-2 lightmap / R5-3 DDGI 未做。**
- 其后排期：R6-2 水体 / R6-3 植被风 / R6-4 地形 / R6-5 动态天空 / R7。

## 编辑器遗留

- **下一项 = `E03` 设置层级**：`RenderSettingsIO.*` 不存在，**5 层覆盖链（偏好→项目→场景→对象→相机）
  没落**。E20 / E26 依赖 E03；E27 依赖 R7-1。**E03 没做之前不要对"最终渲染效果"下结论。**
- **E07 后延的四项**仍未做：子视口降级 / 独立视图模式 / 点击激活 / 视口矩形数组。
- 里程碑：E11.1（Inspector 图标导航条）2026-10-16 收尾；D 组 🟡（E18✅ E19✅ E21✅）；E25✅。

## 已知未解问题（明细）

1. **R6-1 描述符池 under-provision（2026-10-16 已修，⚠ 欠一次全量回归）**
   `ParticleSystem::createDescriptors()` 原来按 `framesInFlight * 8` 给 storage buffer，实际**每帧要 14 个**
   （模拟 4 + 计数 6 + 绘制 4）→ `MYVK_FX=1` **一开就** `VK_ERROR_OUT_OF_POOL_MEMORY`
   （-1000069000 @ `rhi/DescriptorSet.inl:61`）→ `Fatal: Vulkan call failed`。**空场景也一样，与 UI/场景无关。**
   已改 `* 16` + 修正注释。⚠ **池是按 set layout 的"全部 binding"计费**，不是"实际写了几条"。
   修后 `MYVK_FX=1` 跑 120 帧 **VUID/ERROR = 0**。⚠ `engine/**` 在 `GLOBAL_GLOBS` 里 → 这处改动
   "欠一次全量回归"，**等用户指令**。
2. **`verify_particles` 修完后仍 3 项红（31/34）—— 全是"差 1"的口径问题，不是粒子逻辑错**
   - `B1` 冻结帧状态逐位一致（368 != 369）—— 正是"池按 frames-in-flight 分槽"的**固有不确实性**；
   - `F2` 要求 `dispatches > simFrame` 而实测**相等** —— **期望本身很可能就是错的**；
   - `G1b` 排序不改数量（369 vs 368）—— 两次运行取到不同帧。
   ⚠ 这 3 条**此前被 A2/A3 的崩溃一直遮着**（脚本每次都在 A4 抛 `UnboundLocalError` 死掉，B~G 七组
   从未跑到）。**修法是改断言口径，不是改粒子代码**；2026-10-16 那一轮未动。
3. **`verify_bvh` / `verify_csm` / `verify_ibl`** 的全量红已定性为 **harness flaky**（白帧 / 导航球吃了
   右下角点击），修验证侧后单跑全 PASS。**不要再为"凑干净的全量"自动补跑。**
4. **`verify_viewport.py`（2026-10-16 进行中）**
   NO_HUD / 全关两组会话此前**不钉布局与偏好** → 会读外部 `build/bin/config/editor_layout.json`
   （**未被版本控制**，build/ 在 .gitignore 里）。实测证据：同一份二进制在不同分栏比例下与旧基线的差异在
   **526741 / 639043 两个值之间跳**（差 11 万像素），而**两次连续抓图之间永远是 0 差**（V1b 一直 PASS）
   → 抖动全部来自那个外部文件。
   **对策**：加 `pinned_env(td)` 把 `MYVK_EDITOR_LAYOUT` / `MYVK_EDITOR_PREFS_PATH` 指向**不存在的临时
   路径**（= 出厂默认，且 `MYVK_NO_WINDOW_SAVE=1` 保证它永远不被写出）；另 `vp.overlay.stats` 的
   `isBool` 由 false → true（否则落盘成垃圾浮点 `2.369e-38`）。**待重跑确认。**
   若钉死后 `V3/V3b` 仍红，再评估 `tools/vp_nohud_baseline.png` 是否要按"出厂默认布局"重采
   （已发现 HEAD 存档截图与基线只差 4692 px，说明基线本身是同源码期的）。


# 量测三坑（2026-10-16 全量回归查清 · MEMORY.md「量测三坑」条目的全文）

三条都是**测试度量本身的问题**，产品是对的。共同教训：**"断言失败"和"功能坏了"是两件事，
先用"同一个量在不同条件下再测一次"把两者分开。**

## 一、扫描区间必须夹在"被测控件自己的矩形"里

`verify_add_menu` 断言 2 想验"Run/Play/Pause/Stop 四个彩色按钮组水平居中于播放条"，
实现是"在 y∈[TR-BAR.y0+2, y1-2] 这一行上扫**整窗宽度**找高饱和像素（`max-min>45`），
取 `(min x + max x)/2` 与条中心比"。

- 失败输出：`断言 2 (按钮组居中, 组中心 x=1020, 条中心 x=774): FAIL`。
- 用脚本自己存的 `addmenu_00_full.png` **逐像素复算**（脚本 `--size` 默认 1600x900 →
  客户区 **1578x844**，`crect` sanity 的 ±60 容差刚好放行）：
  - 彩色簇 `630..686 / 714..773 / 783..853` = 真正的四个按钮；
  - **x=1321(214,92,94) / 1365(91,214,94) / 1409(91,92,215)** 等距 44px = 右侧
    Inspector 里 Directional Light 段的 **R/G/B 轴标签**；还有一块 x∈[1380,1389]×
    y∈[796,809] 的 10×14 蓝块；
  - `min=630 max=1409 → 1019.5` —— 正是断言报的 1020。
- **那三个簇全在 TR-BAR（296..1253）之外。** 把扫描区间改成 `x∈[TR-BAR.x0, x1]` 后：
  彩色 x∈[630,853] → 中心 742，与 774 差 **33 < 40 容差** → PASS。
- ⚠ 作者其实早知道这条脆（`EditorApp.cpp` 里"时间轴/帧号/倍速"那一块的注释写明了
  "任何彩色控件出现在这一行都会把质心拽偏 → 误报 FAIL"，并为此把那一块**刻意做成灰阶**
  `max-min≈6`）—— 但那只防了**行内**，没防**行外**（右侧 Inspector 的轴标签是故意彩色的）。
- **推广**：任何"在某个 y 带 / 整幅图里做统计"的断言，都要先问"这个范围里还有谁"。
  `verify_viewport` 的采样区必须落在被测物体上、`MYVK_EDITOR_NO_HUD=1` 关掉全部 2D 叠加层，
  都是同一条纪律的不同面。

## 二、比"两个来源的数字"之前先对齐采样窗口

`verify_gpu_timeline` T2e 比"GPU 时间轴面板报的帧总长"与"GPU-TIMING 日志里的 frame"，
断言差 < 5%，实现是两边各自取**中位数**。

- 失败一次（10.19%：面板中位 9.503 vs timing 中位 8.535），**紧接着单跑 PASS** → 按 flaky 查。
- 写三轮采集探针打两侧**原始样本**：

```
run0 panel last=[8.568,8.518,8.557,8.554,8.531]  timing=[8.49,8.52,8.52,8.55,8.55]  → 0.40%
run1 panel last=[11.287,8.512,8.522,8.536]       timing=[8.43,8.54,8.5,8.52]        → 0.22%
run2 panel last=[11.601,8.598,8.524,8.647,8.539] timing=[8.41,8.64,8.65,8.57,8.56]  → 0.33%
```

- 机理：**面板第一条快照在 `frames=7` 就写**（启动期，管线/着色器首次编译 → 实测
  11.3~11.6ms；稳态只有 ~8.5ms），而 **GPU-TIMING 每 30 帧才写一条、压根不采启动段**。
  样本只有 4 条时，这个离群点能把中位数拽高 ~10%。两侧报的其实是同一个量。
- **修法**：丢掉 `frames < 30` 的启动样本再取中位；**阈值仍是 5%** —— 这是把两侧采样窗口
  对齐，不是放宽标准。（剔除启动样本后的三轮实测：0.40% / 0.22% / 0.33%，非常稳。）

## 三、"点了没反应"先怀疑送达，别先怀疑功能

`verify_particles` E1b 点 `Expand all` 后 modules 仍是默认的 4 个，而**同一排**的 `E2b`
（Collapse all）却生效 —— 单看签名很像"按钮坏了 / 状态没写回"。

- 写探针复现，**同一个坐标在两种条件下结论完全相反**：
  - 探针 1（launch → 读 rect → 连点 3 次）：**3 次全无反应**；
  - 探针 2（多一次 `capture_client` + 多读一个 rect，再点 Expand/Collapse/Expand）：
    **三次全部生效**（12 → [] → 12）。
- 结论：按钮没问题，是 `force_foreground` 没抢到前台时**真实鼠标事件被系统丢掉**。
  `verify_asset_drag.force_foreground` 的文档自己就警告过这条："窗口一旦不是活动窗口，
  真实的键盘/鼠标事件就送不进去 —— 症状通常是'点了/按了没反应'……脚本单独跑 3/3 通过，
  跟在别的命令后面连着跑就偶发全挂"。E 组恰好排在 41 个用例之后跑，前台状态最脏。
- **修法**：`click_for(x, y, want, tries=3)` = "抢前台 → 点 → 验状态"，状态不对就再来一次。
  复跑时 E1b/E2b **各一次就 PASS**（输出里会打"点了 N 次"）。
- ⚠ 顺带一条：**"第一次点击被激活吞掉、第二次生效"是这类问题的典型签名**，但**不能当结论** ——
  本轮探针 1 是"三次全败"，所以真正的判据是"换条件再测一次"，不是数第几次成功。


---

# 编辑器打磨七连（2026-10-17）—— 实现级细节

> 规则/结论进 `MEMORY.md`；架构理由进 `docs/editor_architecture.md`；条目化进
> `docs/editor_roadmap.md` §13。这里只放"怎么做的"。

## 主题系统（`engine/ui/ImGuiTheme.{h,cpp}`）

- **提炼**：改动前 `applyVennStyle()` 把"尺寸度量"和"颜色"混在一起写死。拆成两层：
  - **度量**（frame/padding/rounding/scrollbar 等）**不随主题变** → 只在 `applyTheme()` 里设一次；
  - **颜色** → 全部走 `ThemeTokens tok`，`ThemeTokens` 有 ~20 个语义槽
    （`panelBg/panelBgAlt/accent/accentSoft/textDim/textFaint/ok/warn/error/info/axisX/Y/Z/barBg/isLight`…）。
- **5 套主题**：`VennDark`(0) / `UeDark`(1) / `BlenderDark`(2) / `UnityDark`(3) / `Daylight`(4)。
  前 4 套深色 → 底色用 `StyleColorsDark()`；`Daylight` → `StyleColorsLight()`。之后**统一覆盖 ~45 个
  `ImGuiCol_*`**。
- **逐像素不变的保证**：`themeVennDark()` 的每个字段都**逐字照抄**改动前 `applyVennStyle()` 里的值
  —— 包括新加的 `tok.barBg = rgb(0.113,0.117,0.125)`（正是原来三处硬编码的字面量）。
- **派生函数**：`deriveDark(bg,accent,text)` / `deriveLight(...)` 只吃 **3 个锚点**
  （背景 / 强调色 / 文字），其余用亮度插值派生 —— 新增一套主题只需给 3 个数。
- **"深色孤岛"修复清单**（浅色主题下必须逐个处理）：
  1. 顶栏 / 工具条 / 传输条底 → `Token.barBg`（原来硬编码）；
  2. Inspector 左侧图标导航条 `ChildBg` → 按 `isLight` 双分支（浅 0.90 / 深 0.09）；
  3. 视口三段叠加片（modebar / stats / play_hud）→ **只推 `Text`/`TextDisabled`**（浅色主题下文字要变深，
     否则在浅底上看不见）；深色主题**不推** → 逐像素不变。
- ⚠ **主题选择落盘**：走偏好 `theme` 键（int），与语言 `language` 键并列在 Preferences ▸ Interface。

## 语言层（`editor/Localization.{h,cpp}`）

- **接口**：`const char* tr(const char* en)`（英文模式**原样返回指针**，中文模式查表返回译文）；
  `const char* trId(const char* en, const char* id)` 返回 `"译文###id"` —— **ImGui 用 `###` 后的部分做 ID**，
  所以翻译文案变了 ID 也不变，布局不丢。
- **词典** `kZh[]`：`{ "English", "中文" }` 数组，**约 609 条**，按英文原文**精确匹配**（大小写敏感）。
  找不到 → 回退英文原文（**不崩**）。
- **机械包装**：一次性脚本 `_wrap_tr.py` 扫 `editor/*.cpp` 里的 `"..."` 字面量，判断"看起来像 UI 文案"
  就包成 `TR("...")`（宏 = `tr("...")`）→ 共插 **792 处**。
  - 排除：路径、`##`/`###`、printf 格式串、已含 `%` 的、`ImGui::` 的参数名等。
  - **9 处**是拼接表达式（`TR("Opened " + name + ...)`）被误包 → 改成**只翻字面量片段**。
  - **11 处**"包装前本来就是中文"的字面量 → 改回**英文原文** + 词典补中文（因为硬约束要求源码只写英文）。
- **复查口径**：`TR-CJK` 扫描 `editor/*.cpp` 里**未被 `TR()` 包裹**的中文字面量 → **0 处**。
- ⚠ **坑**：`tr()` 返回的指针若来自**临时 `std::string`** 会悬垂 → 词典必须用**静态字面量数组**
  （`const char*`），不能用运行时构造的字符串。

## 启动时序（`engine/core/Window.cpp` + `Application.cpp`）

```
Window ctor        → 不 glfwShowWindow；applyDarkBackgroundBrush()（Win32 GCLP_HBRBACKGROUND 深灰刷）
Application::run   → 逐帧渲染；windowShown==false 时累加 elapsedSec
                     elapsedSec >= kStartupShowDelay(2.0s) → Window::show()
Window::show()     → glfwShowWindow + glfwFocusWindow + VK_LOG_INFO("Window shown ...")
```
- **为什么"隐藏创建 + 2s 后 show"能消白闪**：窗口第一次被映射前，系统用它**自己的背景刷**
  （默认白）填客户区；我们在 `WM_ERASEBKGND` 之前把 class 背景刷换成**深灰**
  （`SetClassLongPtrW(GCLP_HBRBACKGROUND, CreateSolidBrush(RGB(27,28,30)))`）→ 露出来的就是深灰不是白。
  再配合"等首帧真正呈现后才 show"，用户看到的第一眼就是完整界面。
- **幂等**：`show()` 用 `m_shown` 守卫，重复调用无害。`Window` 析构若 `m_shown==false` 也安全。
- ⚠ **自动化兼容**：`MYVK_NO_WINDOW_SAVE=1` 会同时关掉"窗口存档"和"启动缓冲"（`automationRun` 一串），
  于是 `verify_startup_show` 既不能测缓冲、又会覆盖 `imgui.ini` → 新增 **`MYVK_STARTUP_DELAY=1`**
  强制保留缓冲（同时仍不写盘）。

## Hierarchy 行尾两列（锁定 / 可见）

- **布局**：行右侧从右到左 = `[👁 可见] [🔒 锁定]`（先锁后眼，自动化断言 A1 = "眼在锁左边"）。
- **命中修复**：节点加 `ImGuiTreeNodeFlags_AllowOverlap`；两枚 `InvisibleButton` 叠在行矩形右侧。
- **悬停**：不用 `IsItemHovered()`（鼠标移到图标上时节点不再 hover → 会抖），改
  `IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && IsMouseHoveringRect(rowMin,rowMax)`。
- **👁 无条件画**：`vt = w.get<VisibilityComponent>(e)`，`vt==nullptr` 视为**可见 → 画睁眼**；
  点击时 `structuralEdit` 里**按需 `world.add<VisibilityComponent>(e)`** 再设 `visible`。
  （太阳这种懒创建实体因此也能显示眼睛 —— 另一半修在 `Scene::light()` 补组件。）
- **自动化锚点**：`HIER-RECT row0` / `HIER-LOCK row0` / `HIER-EYE row0`（**row0 = 第一行**，
  脚本靠"外扩盒判据"确认两点都能独立命中）。
- **否定测试**：去掉 `AllowOverlap` 重编译 → `verify_hierarchy_status` A2-1/A3-1 **必 FAIL**。

## 偏好 schema 门（`editor/EditorLayoutIO.cpp`）

- 两个**独立**版本常量：布局 `kFormatVersion = 1`（写进布局 JSON 的 `version`）；
  偏好 `kPrefsVersion = 2`（写进偏好 JSON 的 `version`）。
- `loadEditorPrefs`：`json["version"] < kPrefsVersion`（**含缺 version 的老档**）→ **整份丢弃**、
  回出厂默认，并 `VK_LOG_INFO` 打出"seen=vN, need=v2, 已忽略"。
- 这样**改默认值**时才不会被老档顶回（本轮的吸附默认 / 栅格默认正依赖它生效）。

## 二次元着色 A1~A6 + 动画层 B1（2026-10-26 从 MEMORY.md 迁入的细则）

权威 → `docs/anime_gap_review_2026-10-26.md` §2/§3 + 「B1 落地记录」。这里只留**迁移原文**。

### A1~A6 着色

- **A 类槽（ramp/matcap/SDF/detail/aniso/eye）+ 角色光只在 `VENN_BINDLESS` 编译单元里存在**，
  而 `MYVK_BINDLESS` **默认关** → 不开就是"全部静默失效、零报错"。**A 类槽验收必须带它**。
  材质页 **112 B / 7 块**：`params=(steps|−1, matcap, sdfStrength, detailStrength)`／
  `params2=(tiling, rotate, rampEdge, rampSoftness)`／`spare=(useCharLight, sdfShift, anisoStrength,
  anisoShift)`／`spare2=(eyeStrength, eyeIrisDepth, eyeCatchlight, 预留)`。
- **色带/1D LUT 的 `u=0` 会被共享 REPEAT 采样器回绕** → 首末各取一半 = **整条 LUT 平均色**。
  采样前夹到首末纹素中心（`0.5/textureSize(t,0).x`）；查法 = **纯红→纯蓝探针**（混成
  `(0.5,0,0.5)` 就是中招；缺省色带首末都近白，逐像素看不出来）。
  **"白 ramp = 恒等"在 A6 之后不成立**（色带三通道**接管**明暗曲线）→ 恒等律落点是 `params.x = -1`。
- 赛璐璐 **"暗档 ≠ 更暗"** → 判据用**分布塌缩(std)** + **方向性主色**，别用"亮度下降"。
- **A4 各向异性**：切线走顶点 **location 9** → `pbr.vert vTangent` → Kajiya-Kay 双环。
  **`shiftTangent` 必须是 `normalize(T + N*shift)`** —— "先投影再沿法线推"（`T - N*dot(N,T)*s`）
  在 `dot(N,T)==0` 已成立时推的量恒为 0，**shift 变死参数且画面逐像素相同**（像 UI 没接线）。
  `attributeDescriptions()` 必须同步加 9；零长度切线**原样传 0**（`normalize(0)`=NaN）。
- **A3 眼睛**：Unlit 底 = **整段替换**光照和（加法覆盖不了"暗部被压暗"）；虹膜视差在**所有采样
  之前**偏移 `matUV`；眼神光 = 视图空间法线上两个高斯光斑（锁头朝向、不锁光源）。
- **"某特性不吃 X"必须用对照材质自证响应度**：A3 判"翻太阳眼睛 0 px"，而"场景本来对光
  不敏感"时照样 0 px → 同场景必须跑 `eyeStrength=0` 的对照（实测 19.58%）。这是"带对照区"
  在**响应度**维度上的版本（A4 的对照区是空间维度）。
- **相机 yaw 是 `yaw = 180° − φ`，不是 φ**（`forward = -angleDir()`，水平 `(-cos,+sin)`）。
  A3 第一版写反 → **根本没拍到目标**（零暗像素），"0 px 变化"看着像没接线。
  **诊断顺序：先确认被测物在画面里，再谈它动没动。**
- **周期图案（棋盘）判不了"位移方向"**：位移过半格时变化像素数**饱和**，质心又把
  "接缝移动"和"面积变化"混成一个数。判方向要用非周期贴图 + 相位相关（= 要美术资产）。

### B1 动画层 / 骨骼遮罩

- `engine/animation/BoneMask.*` + `ecs::AnimLayer` + `AnimatorSystem::applyLayers`；
  `layers` 空 = **逐位同以前**。**不注册 `FeatureRegistry`**（动画特性，没有 `MYVK_*` 开关，
  入口在 Inspector；硬塞一条只会让"特性数"失真）。
- 遮罩 = **根骨 + 父链子树**，匹配**不分大小写/子串/全收**（不能用 `findJoint`：
  命中即返回 → `hand` 只进一根）；**0 命中 = 该层不生效**（不是"全部"）。
- 顺序硬约束：底座 → **附加层** → `applyPerformance`(LookAt/IK/弹簧骨) → `poseToJoints`。
- 端点**逐位**：`w<=0`=base、`w>=1`=layer（`slerp(q,l,1)` **不是**恒等）；`out` 不可与 layer 同址。
- `dt==0`（编辑器非 Play）**层视为已淡完**，否则刚配好一层在视口里看不见；权重 0 **不清** `playingClip`。
- **手写的 `skinned` 实体在视口里一个像素都不画**（`skinned` 块只建 `SkinnedMeshComponent`、
  只提供 `jointBase`；渲染的是**每个 submesh 的子实体** = Mesh + Material）→ 日志
  `SCENE-SKINNED ok=1` / `ANIM-SKIN ok=1` 全绿而画面全空。验收必须先 `MYVK_EDITOR_IMPORT` +
  `scene.save` 烤模板（`verify_anim_layers.py` 文件头）。唯一同时建两者的地方是
  `scene::instantiateModel`（`verify_model_scale.py` 早记过，10-26 又踩一遍）。
- `anime_girl.fbx` 的 **`Idle` 段数据是坏的**（腿压成尖刺，10-19 `char_import_edit.png` 就是那根
  竖条）→ 底座/上层都用 `Walk`，只按**相位**区分（底座 0.30 s、层 0 s；层的 `time` 不落盘，
  这是唯一能造两个相位的办法）；只有 `Allow=chest` 那组敢用 `Idle` 当上层（坏腿在遮罩外）。
- 像素判据的"上半身/下半身"是**两条带**（视口 **0~28%** 与 **52~100%**）：这个角色是长腿
  chibi、手臂垂在大腿旁，实测 thigh 变化落 0.28~0.72、chest 落 0.00~0.48，**重叠段 0.28~0.48**
  → 任何单条中线都会把手臂和大腿混成一团（第一版 0.52 → "上半身没动"量出 5.23%，判据红实现对）。
  换机位/换角色必须**重量这两个比例**。
- **"变化像素数随权重单调"不成立**（0.5 → 6404 px、1.0 → 6356 px，反了 0.7%）：变化像素是
  **集合**量，对位移会饱和。改成"0.5 与 1 不是同一张图（5.75%）**且** 0.5 与基线也不是
  同一张图（5.61%）"；"0.5 恰好一半"交给数值用例（`mask_w05_scale=2.0`，逐位量）。
- 否定测试：去掉遮罩（`Mask=All`）→ 上半带 0.000% → **3.903%**，③ 会红。判据有判别力。
