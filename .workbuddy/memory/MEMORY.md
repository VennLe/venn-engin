# venn —— 项目长期笔记

> 2026-10-09 起项目更名为 **venn**（原 MyVulkanEngine），**仓库目录 = `E:\code\cpp\venn-engin`**
> （CMake 工程名是 `Venn`，两者不一致是刻意的）。工程里**只有一个可执行文件
> `Editor.exe`**：内置 demo 与独立的 Sandbox 示例程序已按用户要求**全部删除**，
> venn 启动不再加载任何样例（`resetToEmptyScene`）。
>
> **2026-10-10 起世界坐标系 = 右手系 Z-up**（与 Blender/3ds Max 同向，用户要求）。
> 相机 up、栅格地面（z=0）、贴地方向（min.z）、glTF 导入转换（绕 X+90°，
> 见 MeshLoader `kYupToZup`；OBJ 不转）都基于此。详见 2026-10-10 日志。
>
> **改目录名后必须删 `build/` 重新 configure** —— CMakeCache.txt / VerifyGlobs.cmake 里
> 全是绝对路径，直接 `cmake --build` 会报 "CMakeCache.txt directory ... is different"。

## 项目定位与分层（新增代码必须放对层）
从零手写的 Vulkan 引擎（学习向，C++17）。
`engine/core`(Application 主循环/Window/Input/Logger/Time/WindowGeometry) → `engine/rhi`(Vulkan 封装) →
`engine/render`(Renderer 组织一帧/HdrTarget/ShadowPass/PostProcess/FrameResources) →
`engine/scene`(Scene/Camera/Light + SceneSerializer) → `engine/assets`(Mesh/Texture/Material/加载器) ；
还有 `engine/ui`(ImGuiManager)、`engine/ecs`(World/Entity/Components)。
应用层只剩 `editor`(EditorApp + 面板 + 自研脚本语言 + 编辑器态场景)。原来并行的
`game`(SandboxGame) 已删除。依赖方向 `editor → render/ui → scene/assets → rhi → core`；
**engine/ 不认识 editor/**，唯一接缝是 `Application::activeScene()` /
`Renderer::setViewportSize` / `viewportTextureId`；一次性收尾走 `Application::onShutdown()`。
**CMake 目标名**：静态库 `venn`（`project(Venn VERSION 0.2.0)`）+ 可执行 `Editor`。

## 构建与运行（Windows + MinGW）
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe \
  -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe \
  "-DCMAKE_POLICY_VERSION_MINIMUM=3.5"   # 必须（CMake4 拒绝 GLFW 的 3.4）
cmake --build build -j 8
```
Vulkan SDK 头文件在 `E:/code/vulkan/core/Include`。只有一个目标 `Editor`；资源拷贝
（shaders/models/scenes/scripts/fonts/icons）由 CMake 函数 `myvk_copy_runtime_assets(target)` 统一负责。

**Makefile**（`Makefile` + `make.cmd`，驱动 `mingw32-make`）：`build / run / clean / rebuild /
distclean / package / smoke / info / help / install-make-alias`。
**`make run` = 打开编辑器**（唯一入口）。**没有 `sandbox` / `play` 目标** ——
Sandbox 已删，游戏直接在编辑器视口里跑。
- 配方须**同时兼容 cmd.exe 与 sh** → 只用 `cmake -E`、正斜杠、`cmake -E env`
- `echo` 与 `.cmd/.bat` **整文件**必须纯 ASCII（cmd 用 GBK，否则乱码被当命令 → 死循环）
- 配置戳 `build/.configured-$(BUILD_TYPE)`；`:=` 立即展开变量必须定义在依赖者之前
- `make run` 用 `cmake -E chdir build/bin`（ImGui 把 imgui.ini 写在 CWD）；`package` 用 `cmake -E tar`

**环境变量**：`MYVK_FRAMES=N`(跑 N 帧退出；**同时抑制窗口几何存档**) /
`MYVK_EDITOR_PLAY=1`(编辑器启动即 Play) / `MYVK_NO_WINDOW_SAVE=1`(禁止写窗口几何存档) /
`MYVK_VALIDATION=0` / `MYVK_SHADER_DIR` / `MYVK_ASSET_DIR` /
`MYVK_SCENE_TEST=1` / `MYVK_FREEZE_ANIM=1` / `MYVK_HIDE_UI=1`(关全部 UI，像素对比必备) /
`MYVK_GRID=0`(关视口地平面栅格；默认开) /
`MYVK_CONTENT_DIR=assets/models`(Content 面板初次打开进哪个目录，默认 assets) /
`MYVK_LOG_RECTS=1`(打印关键控件矩形，给自动化脚本定位用；见"工具") /
`MYVK_SHADOW_DBG=1`(每帧打阴影 casters/center/radius/lightDir；排查地面暗区/阴影异常第一步) /
**编辑器自动化钩子（2026-10-09 新增，只在设了变量时生效）**：
`MYVK_EDITOR_SCENE=<路径>`(启动即打开该场景而非空场景) /
`MYVK_EDITOR_SELECT=<名字>`(启动后按名字选中实体，让手柄/检查器有内容) /
`MYVK_EDITOR_GIZMO=move|rotate|scale`(启动即切手柄模式) /
`MYVK_EDITOR_PREFS=1`(启动即开 Preferences 大窗口，截图验证用) /
`MYVK_EDITOR_CAM="x,y,z,yawDeg,pitchDeg"`(把视口相机摆到指定位姿 —— 截图/交互测试用。
**必须排在 `MYVK_EDITOR_SCENE` 之后**，见坑点 #53) /
`MYVK_MSAA / LIGHTS / CLUSTER_CULL=0 / DEPTH_PREPASS=0 / LIGHT_GIZMOS=0 / TILE_SIZE /
CLUSTER_SLICES / MAX_LIGHTS_PER_CLUSTER / CLUSTER_STATS=1`。

## 渲染管线约定（改动前必读）
Pass 顺序：`ShadowPass → DepthPrePass(独立 RP) → ClusterBuild(compute) → Forward(MSAA resolve，
深度 LOAD) → PostProcess(Bloom+Tonemap) → 交换链`。**compute 不能在 RP 内部执行** → 深度预通道必须
独立成 RP；该 RP **无条件执行**（只把"画几何"包进开关），否则前向 RP 的 `LOAD` 深度未定义、画面废。
- **Forward 内部子顺序**：`不透明 → 地平面栅格 → 透明`。栅格夹在中间是**刻意的**：吃深度测试
  （墙/柱子/箱子能正确挡住它）+ 不写深度（玻璃能叠在它上面）+ 排在透明之前。
  它**绝不能**做成 ImGui 叠加层 —— ImGui 永远压在 3D 之上，栅格会穿透一切。
- **阴影（方向光，`ShadowPass`）三条硬约定**（2026-10-10 补齐，之前全是 bug）：
  1. **投影矩阵必须是 Zero-to-One**：`glm::orthoRH_ZO`（不是 `glm::ortho`）。Vulkan 的 clip z 是
     [0,1]，GL 约定的 [-1,1] 会让"写入阴影图的深度"和 `pbr.frag` 采样时算的参考深度不在同一空间
     → 覆盖区内一切表面被判成阴影（屏幕上一块边界笔直的暗四边形）。
     对应地 `pbr.frag::shadowFactor` **只对 xy** 做 `*0.5+0.5`，z 直接用。
  2. **正交范围用"投影物体的世界 AABB"**（`Mesh::boundsMin/Max` 8 角过世界矩阵，
     `forEachShadowCaster` 遍历），**不能用实体原点**——原点会把包围盒退化成 1 个点（radius 只剩
     2m），阴影贴图只覆盖原点周围一小块，屏幕上就是一块方形边界。无投影物体时以
     `camera().target()` 为中心给 25m 兜底。
  3. **"只接收不投射"的物体必须 `VisibilityComponent::castShadow=false`**（自带地面就是），
     否则 120×120 地板会把正交范围撑到 ±85m（深度精度全浪费）+ 地板自投影 acne。
- **缺省贴图的色彩空间**：`Renderer::createDefaultTextures()` 的白色（ORM）与平坦法线
  **必须 `makeSolid(..., srgb=false)`**（UNORM）。建成 SRGB 会被硬件 gamma 解码：平坦法线
  (128,128,255) → (0.216,0.216,1.0) → `nSample=(-0.568,-0.568,1.0)`，即"平坦法线"被整体扳了约 39°，
  大面片上 TBN 一退化就整块变色（又是一条硬边）。颜色贴图（albedo/emissive）才 SRGB。
- 描述符 **set 0**：0 FrameUBO / 1 shadowMap / 2 lights / 3 clusters / 4 lightIndices / 5 clusterStats
  （灯数据全挤进 set 0：Vulkan 只保证 4 个 set 同时绑定）。set 1/2/3 = albedo/normal/ORM。
- compute 管线复用 `m_globalSetLayout` → dispatch 必须绑 `frame.globalUBOSet`。
- 推常量 **112B**：`mat4 model + vec4 baseColor + vec4 pbr(metal/rough/normalScale/ao) + vec4 emissive`
  （`render::PushConstants`；栅格另有 48B 的 `GridPushConstants`）；顶点 `pos3+normal3+uv2` stride 32。
- ⚠️ **旁路管线必须让 push constant range 与主管线逐字段相同**：set 0 是整个 Pass 一次性绑定的，
  而 Vulkan 判"管线布局兼容"时**把 push constant range 的范围与阶段也算进去**。若新管线只声明
  自己的小 range（栅格本来想声 48B），立刻报 VUID-...-08600「set 0 被不兼容的
  vkCmdBindDescriptorSets 打断」，**后续所有 draw（含透明）全报错**。→ 栅格管线**故意**声明与
  主前向管线一样的 112B（实际只推 48B，合法）。别想着"换个 layout 重绑一次 set 0"——那会
  打断 set 0 的既有绑定，反而更糟。
- 交换链/纹理均 SRGB，**着色器内不做手动 gamma**；视口/剪刀动态，resize 不必重建管线。
- 绕序：`addFace(n,u,v,half)` 中 **u×v 必须等于 n**，正面 `COUNTER_CLOCKWISE`（配 `projMatrix` Y 翻转）；
  `offsetAlongNormal`：立方体面 true，单个面（地面）必须 **false**。

## 分簇光照（Clustered Forward）
- **簇编址三处必须一致**（C++ / `cluster_build.comp` / `pbr.frag`）：
  `idx=(slice*gridY+tileY)*gridX+tileX`，切片 `d(k)=clusterNear·(zf/clusterNear)^(k/slices)`。
- 切片起点用 `clusterNear` 而非 `zNear`（glm::perspective 未定 ZERO_TO_ONE，NDC z∈[-1,1]）。
- **固定槽位** `lightIndices[ci*cap+i]`（非原子追加）→ 结果**可复现**，截图对比才有意义。
- `maxLightsPerCluster` **唯一数据源是 C++**（经 `ubo.lightParams.w` 传给片元），**严禁在
  pbr.frag 硬编码**。只花显存不花时间 → 默认 **128**。
- 射灯只按 range 球剔除（锥角在片元算）；衰减用窗口化平方反比 `win²/d²`，range 处严格归零。
- 透明（Blend）不写深度、按相机距离**降序**排序、**不参与深度预通道**。
- 遥测 `stats` SSBO：0 单簇最大 / 1 溢出簇数 / 2 配对数 / 3 空簇数（`beginFrame` 回读并清零）。

## editor/ 编辑器层约定
三条铁律：**① 编辑器模式不跑游戏逻辑 ② 渲染目标是 ImGui 子窗口而非全屏 ③ 编辑态与运行态分离**。
- **运行态 = 同进程的内存副本**（`m_runtimeScene`，最核心）：`EditorContext::activeScene()` 是唯一
  分派点 —— `usingRuntime() ? m_runtimeScene : m_editorScene`。点 Play 把编辑态
  `sceneToJson` → `sceneFromJson(m_runtimeScene)`（**内存拷贝，不落盘、不起进程**），
  于是**视口自己就变成了游戏画面**（复用同一条离屏渲染链，不需要第二个窗口）。
  - **不做跨进程**：曾用 `Editor.exe --play <快照>` 另起进程弹独立游戏窗口（`GameProcess`/`PlayerApp`），
    已**整体删除**。原因：用户要的是"就在原来视口里显示"；同进程零渲染器改动即可达成。
  - 运行态相机：`play()` 里对**副本**调 `setUseGlobalInput(false)` + `setFlyMode(true)`；
    **此后编辑器不再驱动它**（视口在 Play 期间不处理任何键鼠导航），怎么动由游戏逻辑决定。
    编辑态相机不受影响（详见"视口相机与导航"一节）。
  - `pause()` 只翻状态；`stop()` 丢弃 runtime scene、复位 `m_gameFullscreen`、`validateSelection()`。
  - 脚本只在 Play 态跑，且只作用于 **runtime scene**；编辑态只 `preloadScripts()`（编译不执行）。
- **Play 期间编辑器只读**（gizmo/Inspector/层级增删被 `!isEditing()` 挡住 —— 那边跑的是副本，
  改了也同步不回去；想做"边跑边编辑"要另开一轮设计）。
- **命令模式**：值编辑 `RawBytesEditCommand`，结构编辑 `SceneSnapshotCommand`（整场景 JSON）；
  统一入口 `structuralEdit(name,fn)`（捕 before → 执行 → 捕 after）。破坏性操作**两段式**
  （ECS 遍历期间不能增删 → 攒 pending 队列，树画完再 `flushPending()`）。
- **视口解耦**：`setViewportSize` 让 3D 输出写离屏图，`viewportTextureId()` 给 `ImGui::Image`。
  纹理稳定性：draw data 在 drawFrame 前已构建 → ① recreate 保留场景输出图 ② `createViewportTextures`
  比较未变则不重注册 ③ ViewportPanel 尺寸刚变那帧不画图（`m_resizePending`）。
- **布局**：SplitLayout 二叉分栏树（比例 0..1 → 自适应）；主体面板必须 **Always 落位 +
  NoMove/NoResize**；分隔条用前景 DrawList 画线 + 手动命中。标题带状态须加稳定 ID（`###viewport`）。
- **自研脚本 .vks**（`editor/script/ScriptEngine.{h,cpp}`）：`ScriptHost{scene,entity}` 是唯一接缝
  （安全性是结构性的）；热重载 mtime 轮询 0.5s，**编译失败保留上一版**；数值用 double，
  `rotate` 单位是**度**，步数上限防死循环。
- **全局观感走 `ImGuiManager::applyVennStyle()`**（`init()` 里 `StyleColorsDark()` 之后调）：
  尺寸 `FramePadding(9,5)/ItemSpacing(9,7)/WindowPadding(10,8)/圆角 4px`；配色 accent
  `(0.16,0.53,0.94)`、`WindowBg(0.106,0.110,0.118)`、`MenuBarBg == TitleBg(0.113,0.117,0.125)`
  —— 工具条刻意用同一个颜色，和菜单栏**连成一条顶栏**（UE 观感的关键）。
- **面板宽度按窗口宽度自适应**：`kLeftFrac 0.175 / min 300 / max 520`、`kRightFrac 0.205 / min 330 / max 560`；
  用户拖过分隔条后 `m_userAdjusted=true` 不再自动重算，仅"首帧或宽度变化 >1.25×/<0.8×"时重算。
  （原因：最大化 3840 时固定 320px 侧栏只占 8%，挤成一坨。）
- **快捷键**：`F2` 重命名（Hierarchy / Content 各自处理，Content 只在鼠标悬停本面板时生效）、
  `Ctrl+D` 复制、`Ctrl+N` 新建场景、`Delete` 删除、`Backspace` 上一级。
- **Content 面板的右键菜单是"显式单 popup"**：`DrawEntry` 只写 `m_ctxRequest/m_ctxEntry`，
  `drawGrid()` 循环外 `OpenPopup(kEntryCtxPopup)` + `drawEntryContextMenu()`；`BeginPopup`
  返回 false 就自动把 `m_ctxOpen` 清掉（状态自愈，不需要额外关闭逻辑）。见坑点 #30。

### Shift+A 添加菜单 / 播放条 / Preferences（2026-10-10 新增）
- **Shift+A 菜单**（视口 hovered 才触发；按住右键驾驶相机时不抢键）：popup 每帧无条件提交；
  落点 `m_addSpot` = 鼠标射线 ∩ z=0（朝天兜底视线前方 5 米）。Mesh 子项 = primitive 贴地
  （`createPrimitiveOnGround` 抬半高）；Light 子项里 **Sunlight = 场景级太阳**（唯一、有阴影、
  日照预设、`Scene::lightEntity()` 访问器，太阳不进实体序列化）；**Directional Light = 实体级
  补光**（`LightInstance::Type::Directional=2`：`positionRange.w=1e5` 影响球 trick 让分簇剔除
  全覆盖，`collectLights` 跳过太阳实体，pbr.frag `shadeLocalLight` 按 `cosOuterType.y>1.5`
  分支取 `Ld=normalize(-direction)`、att=1）。
- **播放条**：`LayoutRects.transport`，`computeLayout` 从视口底部扣 `kTransportH=34px`
  （3D 渲染高度自然缩小），`EditorApp::drawTransportBar` 居中画 Run|Play|Pause|Stop；
  Content 底行仍是统计文字（播放条曾在这里，已还原）。
- **Preferences**：`drawSettingsPanel`，920x620 居中可缩放，左 168px 分类栏
  （Interface/Viewport/Editing/Keymap/Scene）+ 右内容页，apply 不持久化。
  窗口 ID 是 `###prefs`——**别沿用旧名**（imgui.ini 残留旧窗口位置会让它飞到角落，见坑点 #43）。

### 编辑器锁定 + 数字键 0「落地」（2026-10-10 新增）
- **`ecs::LockedComponent`（纯标记）**＝"看得见、动不了"。两处生效：
  `PickingSystem::pick` **跳过**它（点空地 = 不选中任何东西，而不是选中地面）、
  `GizmoController::update` 直接 return（挡住"从层级树选中"这条路径）。
  序列化字段 `je["locked"]`（**读写都要补**，否则存档再打开地面就自动解锁了）。
  出口只有一个：Inspector Transform 段的 `Locked` 勾（走 `structuralEdit` 可撤销），
  锁定期间整段变换字段 `BeginDisabled`；Hierarchy 后缀 `[locked]`。
  自带 `Ground` 默认带这个组件 —— 它是编辑器的参照系，**别让它参与点选/变换**。
- **数字键 0 = 落地**（`ViewportPanel::dropSelectionToGround`）：把选中物体**竖直**挪到地面，
  只改 z、让**世界包围盒底部**贴到 z = 0（用 `PickingSystem::worldAabb`）。
  两条硬约束：① 只认"**在 3D 视口里左键点中**"的选中（`EditorContext::selectionFromViewport()`
  —— `selectFromViewport()` 置真，`select()` / `clearSelection()` 置假），层级树选中时只提示不动手；
  ② 只对有网格的实体生效（灯/相机没有包围盒）。位移是世界空间的，父节点下要换算回父空间
  （`inverse(parentWorld) * vec4(0,0,dz,0)`），入栈走 `TransformEditCommand`。

### 碰撞体系统（2026-10-10 新增，`engine/physics/` 四文件 + `editor/ColliderView`）
- **QuickHull**（`ConvexHull`）三维凸包，必须配冲突表（UV 球级输入否则 15 亿次距离计算）；
  退化输入（Plane 共面）兜底成 2cm 薄板。**GJK+EPA**（`ConvexCollision`）不依赖绕序：
  面朝向用对顶点定向 / `n·v>0`。支撑函数 `support_{M·S}(d)=M·support_S(Mᵀ·d)` → 非均匀缩放精确。
- **`CollisionComponent` 与 TransformComponent 字段名/类型/欧拉序完全一致** → 手柄只换指针
  （`GizmoController::TrsPointers` + `m_onCollider`，世界矩阵 = 实体世界 × `cc.localMatrix()`）。
  序列化只存 7 参数，凸包顶点读盘后按 mesh 重建（不落盘）。
- **"刚好包裹" = 顶点按 AABB 中心居中**（否则缩放手柄往外拉而非绕自己放大）；胶囊半径取
  另两维半长的**外接圆**半径（内切会露角）。入口：视口右键菜单（按下/松开 <6px 才算点击，
  与右键转相机共存）；`MYVK_EDITOR_COLLIDER=hull|capsule` 自动化钩子。
- **碰撞框线框走 ImGui draw list 不走 3D 管线**（免描述符同步 + 天然 X-ray）；
  凸包"边"从面提取（顶点两两连会糊成一团）；只读遍历用 const `w.each<T>`。
- **编辑态碰撞不阻止变换，只在运行态分离**（`runCollision`：`resolveBodyOverlaps` 4 轮双向推半、
  推完立刻重建这俩 collider；`resolveCamera` 半径 0.25 球推出；在 runRuntimeScripts **之后**）。
  日志预算 `m_collisionLogBudget`（前 240 帧逐条）—— 初值/复位为 0 会让 Play 全程静默。
- **日志是自动化的"接口"：追加字段只能放行尾**。`gizmo begin/end` 的 `target=` 插在中间
  打断了 verify_gizmo_scale.py 的正则（`space=` 必须紧跟 `axis=`）。新脚本用 `.*target=` 适配，
  END 组编号 pos 3-5 / rot 6-8 / scale 9-11 / target 12。
- 脚本：`verify_collider_menu.py`（29 断言）/ `verify_collider_gizmo.py`（51 断言，**三模式各起
  一个进程**——W/E/R 在 flying 时让位，别靠按键切模式）/ `verify_collision_play.py`。

### Hierarchy 的"文件夹"节点（2026-10-09 新增）
- **显式标记**：`HierarchyComponent::group`（**不要**靠"这个节点没有网格组件"去猜 —— 空节点会被误判）。
  序列化字段 `je["folder"]`；右键 `Mark as Folder` / `Unmark as Folder`；新建走 `uniqueName()`
  保证 `Folder` / `Folder 2` / … 唯一；`duplicateEntity` 要连 `group` 一起复制。
- **树行的图标不能塞进 `TreeNodeEx` 的 label**（label 是纯文本，字体里没有文件夹字形）→
  label 传空串，图标 + 名字全部自己用 `DrawList` 画：
  `行左边缘 = ImGui::GetCursorScreenPos().x`（**必须在 `TreeNodeEx` 之前取**）
  `文本起点 = 行左边缘 + ImGui::GetTreeNodeToLabelSpacing()`
  （后者实现 = `FontSize + FramePadding.x * 2`，**恰好等于** imgui_widgets.cpp 里的 `text_offset_x`）。
  文件夹名字用暖色 `IM_COL32(238,202,132,255)`。图标在 `editor/EditorIcons.h`
  （`drawFolderIcon` 52px 给 Content / `drawTreeFolderIcon` 小号给 Hierarchy）。

### 模型导入 = "根节点 + 子网格"，落点贴地（2026-10-09 新增）
- `EditorScene::instantiateModel()` 返回 `ImportResult{root, entities}`：**一定要建一个空根节点**
  当把手 —— glTF 节点的 `transform` 是**世界变换**，直接分解成 TRS 塞进子节点之后，
  它们彼此没有共同父级，没法"整体平移到落点"。
- `EditorScene::alignImportToGround(scene, picking, imported, target)`：联合所有子网格的世界 AABB →
  `shift = (target.x - c.x, target.y - box.min.y, target.z - c.z)`。
  **拿不到 CPU 侧包围盒时只平移、不贴地**（宁可不贴也不要塞到地面以下）。
  会打一行 `alignImportToGround: box.min.y=… bottom=… (target.y=…) root=(…)` 供自动化断言。
- 两个入口共用这套逻辑：**拖进视口**（落点 = 鼠标射线 ∩ y=0 平面）与 **双击 Content 里的模型**
  （落点固定为世界原点）。
- Content 侧**只有模型能拖**（`.gltf/.glb/.obj`）：`BeginDragDropSource` 必须带
  `ImGuiDragDropFlags_SourceAllowNullID` —— 缩略图条目是无 ID 的 item，不带会 `IM_ASSERT(0)`
  直接崩（退程码 `0xc0000409`）。视口侧拖拽中画绿色高亮边框 + 落点说明文字。

## 视口相机与导航（先分清"谁的相机"）
**两者的存储模型相同（`scene::Camera` 的自由飞行），但归属完全不同 —— 这是最容易做错的地方：**
- **编辑态 = 编辑器视口的导航相机**，由编辑器**完全驱动**：视口悬停时按 UE 编辑器的习惯操作。
- **Play 态 = 游戏自己的相机**，编辑器**不碰**：视口在 Play 期间**不响应任何键鼠导航**
  （用户明确要求"运行前的视口要好用，运行后归游戏管"），只负责渲染 + 全屏切换。
  ⚠️ 上一版把导航做进了 Play 态，方向是错的。

### 编辑态导航（完全按 UE 编辑器）
| 操作 | 行为 |
|---|---|
| 按住**右键**拖动 | 自由旋转（水平 + 俯仰） |
| 按住**中键**拖动 | 平移 |
| **单独滚轮** | 沿视线**推 / 拉相机**（`kNavDollyPerNotch=0.6f` 米/格 × sens） |
| **右键 + 滚轮** | 调导航灵敏度（±`kSensitivityStep=0.1f`/格） |
| **右键 + WASD** | 前后左右飞行（**只在按住右键时**） |
| **右键 + Q / E** | 下降 / 上升；**Shift** 加速 ×3 |
| **左键** | 点选物体 / 拖 gizmo 手柄 —— **不参与导航** |
| **左下角导航球** | 拖拽 = 绕注视点转视角；点轴端小球 = 吸附正视图（见下） |
| **数字键 1/3/7** | Front/Right/Top（Ctrl+ 反向 = Back/Left/Bottom）；2/4/6/8 = 15° 步进 |

- **滚轮必须有两条路，靠"是否按住右键"分流**（用户明确要求）：单独滚轮 = 推拉相机，
  右键 + 滚轮 = 灵敏度。判定复用 `lookDown`，和 WASD 门控同一个开关，所以不会有第三种语义。
  HUD 上的滚轮入口（鼠标停在浮层里）**同样要求按住右键**，保持一致。
- **"飞行只在按住右键时生效"是必须的**：`W/E/R` 同时是手柄模式快捷键，不门控就会
  "按 W 往前飞的同时把手柄切成平移模式"。UE 就是这么分的；`EditorApp::handleShortcuts`
  里用 `const bool flying = IsMouseDown(Right)` 门控那三个快捷键。
- 常量在 `ViewportPanel.cpp`：`kNavLookPerPixel=0.0025f` / `kNavPanPerPixel=0.01f` /
  `kNavMoveSpeed=5.0f` / `kNavDollyPerNotch=0.6f` / `kSensitivityStep=0.1f`。
- `EditorContext::applyEditorCameraMode()` = `setUseGlobalInput(false)` + `setFlyMode(true)`。
  **每次场景重建都必须重设**：新建/打开/重建 demo → `onSceneReplaced()`（内含调用），
  undo/redo（整场景快照恢复）→ 各自入口里显式调用。**飞行模式不在序列化范围内**，
  漏掉就表现为"导航突然变回老式轨道相机"。

### 导航球（Navigation Gizmo，视口左下角，Blender 风格）
- `ViewportPanel::drawNavGizmo`：六个轴端小球（正轴实心+白字母 / 负轴暗底+轴色字母）
  按深度（`-d·f`）远→近排序绘制；整块方形区域是一个 `InvisibleButton`，
  **四角也吞鼠标**（不穿透点选，和 Blender 一致）。Z-up 后**蓝 Z+ 球 = Top**。
- **切视图/拖拽都"绕当前注视点"转**：`snapViewTo` / `orbitViewAroundPivot` 都是
  先存 `target()`，改完 yaw/pitch 再 `setTarget(pivot)` 钉回去 —— 原地转头会让
  被观察物飞出视野。顶/底视图 yaw 是退化量，**保留当前 yaw**。
  Z-up 视图映射：Front=相机在 **-Y**（yaw=+π/2）/ Back=+Y / Right=+X（yaw=0）/
  Top=+Z（pitch=+kPitchLimit）。
- 点击 vs 拖拽：位移 ≤ `kGizmoClickSlop(5px)` 算点击（吸附），否则算拖拽。
- 数字键接入点在 `EditorApp::handleShortcuts` 里 **`io.KeyCtrl` 分支之前**
  —— 那个分支末尾 return，放后面会把 Ctrl+1/3/7 吞掉。
- 主键盘数字键的 ImGui 枚举名是 `ImGuiKey_1`（不是 `ImGuiKey_Keypad1`，那是小键盘）。
- **`Camera::kPitchLimit = 1.5533f`（89°）**：原三处硬编码 1.52f 会让顶视图偏斜；
  89° 是 `forwardAxis()/rightAxis()` 兜底分支的安全上限，别再加大。
- 调试：`MYVK_LOG_RECTS=1` 下打 `NAV-CAM yaw=... pitch=...`（变化 >0.02 rad 才打）；
  交互验证脚本 `tools/verify_nav_gizmo.py`（Z-up 后点 **Z+** 球 / 按 1/3/7，四条断言）。

### ⚠️ Camera 飞行模式的不变式（改任何 setter 前必读）
`position()` 与 `m_target` 通过 `target = position - angleDir()*distance` **互为逆运算**。
所以**任何动 target / distance / yaw / pitch 的接口都必须同步另一侧**，
否则飞行模式下表现为"拖了没反应 / 画面静止 / 绕空点打转"：

- `syncFlyTarget()`（位置→target，`moveLocal`/`look` 用）
- `setTarget` / `setDistance` / `setYaw` / `setPitch`（target→位置方向，反推 `m_flyPos`）
- `orbit` / `zoomBy` / `panWorld` / `dolly`（轨道风格入口，飞行模式下自动改写成等价动作）
- `Camera::update()`（全局输入路径）也要按 `m_fly` 分支，否则与上面的入口脱节

新增任何改相机姿态的接口，都要问一句"飞行模式下这条还成立吗"。

### 灵敏度与浮层
- **导航灵敏度**（`EditorContext::m_navSensitivity`）：默认 **1**，clamp `0.05..20`，
  移动与旋转**共用**。视口右上角 HUD 可 `InputFloat` 直接填（≤0 忽略）、
  **按住右键 + 滚轮**可调（上滑变大 / 下滑变小）。**只作用于编辑态**。
  （单独滚轮是推拉相机，不碰这个值 —— 见上面的导航表。）
  InputFloat **step 传 0** 就没有 +/- 按钮（用户明确不要按钮）；框宽
  `kSensitivityFieldW=88` 够显示 "20.00"。
  **2026-10-10 起浮层只保留**：标题 `Viewport navigation` + Sensitivity 输入 + `Ground grid` 勾选框；
  操作提示文字（RMB/MMB/WASD…）与 Sensitivity 的 `Reset 1.00` 按钮**已按用户要求删除**，别再画回去。
- **Play 态浮层**只有"全屏 / 恢复"按钮 + 播放状态，**刻意不含导航控件**。
- **浮层必须是 viewport 的 child window**（`SetCursorScreenPos` + `BeginChild`，在
  `ImGui::End()` **之前**画）：ImGui 每帧把聚焦窗口提到最前，独立顶层窗口会被 3D 图盖住。
- **浮层尺寸必须按字体度量算**（`CalcTextSize` / `GetFrameHeightWithSpacing` /
  `GetTextLineHeightWithSpacing` + `GetStyle().WindowPadding`）。写死像素的话，
  用户在 Settings 里放大字号后文字就被裁掉半截。也**不要**在视口工具栏里再塞全屏按钮 ——
  那条工具栏本来就很长，会被挤出右边缘（实测被裁成 "Fulls…"）。

## Gizmo 拖拽与吸附（`editor/PickingSystem` + `editor/GizmoController`）
**两个曾经的致命缺陷（用户报"拖拽方向反 + 速度越来越快"），已修，改这里前必读：**
- **① 轴线/射线最近点的参数符号**（`PickingSystem::closestOnAxis`，Ericson 5.1.9）：
  `s = (b*e - c*d) / (a*c - b*b)`。曾把分母写成 `b*b - a*c`（差一个负号）→ 参数整体取反
  → **往右拖物体往左跑**。现在分母保留 `b*b - a*c` 只用于判平行，参数用等价的
  `(c*d - b*e) / (b*b - a*c)`。**改符号前先想清楚分子分母是不是同时翻了。**
- **② 拖拽参考线必须"冻结"**（正反馈环）：`applyDrag` 若每帧都用物体**当前**位置当轴线原点，
  则物体自己走过的位移会被重新算进参数 → `D_new = Δ + D_old` → **越拖越快**（指数发散）。
  正确做法：`beginDrag` 里快照 `m_dragOrigin / m_dragAxis / m_dragWorldLen`，`applyDrag`
  全程只用这份快照（`m_origin / m_axisDir / m_worldLen` 每帧都在变，不能碰）。
- **三套独立的吸附**（`EditorContext`）：`snapMove/snapMoveStep`、`snapRotate/snapRotateStep`、
  `snapScale/snapScaleStep`，各带 `...Ref()` + `snapForCurrentMode()/snapStepForCurrentMode()/
  toggleSnapForCurrentMode()`。**量化语义各不相同，别统一**：
  | 模式 | 量化对象 | 默认步长（对齐 UE5） |
  |---|---|---|
  | 平移 | **位移增量**（米） | 0.1 m（UE5 的 10 uu → 本引擎 1u=1m） |
  | 旋转 | **角度增量**（度） | 10° |
  | 缩放 | **倍率 factor**（不是缩放绝对值！） | 0.1 |
  缩放量化倍率的原因：量化绝对值会让第一下拖拽从 0.37 直接跳到 0.4，像抽搐了一下。
- **UI**：视口工具栏**第二行** `drawSnapRow()`（`snapField()` 勾选框 + DragFloat + Presets 弹窗 +
  `current: … snap on/off`）；**G 键** = 切换当前工具的吸附（`!flying` 门控内）。
  拖拽时右上角浮层显示实时读数 `Move X: +0.600 m  [snap]`（`GizmoController::readout()`，
  只在 `dragging()` 期间有效，`endDrag`/非拖拽帧会被清空）。
- 旋转**始终在"拖拽起始姿态"上叠加**（`R = D*R0` 或 `R0*D`），不做增量累乘。

### 手柄的三种粒度（2026-10-09 重写：从"只有单轴"扩到 7 种 `Handle`）
`enum class Handle : int { None=-1, AxisX/Y/Z=0/1/2, PlaneYZ=3, PlaneXZ=4, PlaneXY=5, Screen=6 }`。
- **平面枚举名 = 该平面的法线**（`PlaneYZ` 的法线是 X）。颜色按**法线**取 → 法线 X 的方片是红的，
  和 UE / Blender 的"红方块 = 法线朝 X 的面板"一致，不用记额外规则。
- 平移/缩放：单轴（沿线/单轴拉伸）+ 平面双轴（平面内自由移动 / 两轴一起缩放，取两腿平均）
  + 中心 `Screen`（**视平面**内自由移动 / **等比**缩放）。旋转：三轴环 + 外圈 arcball 自由旋转。
- 平面方片画在两根轴夹角处（`kPlaneT0 0.30 ~ kPlaneT1 0.62` 相对手柄长度），命中用
  `pointInQuad()`（四角叉积同号），**不是**逐边求交。命中顺序：旋转先判三轴环再判外圈
  （正交视图下"视轴环"会和某根轴环重合，应优先给具体轴）；平移/缩放的顺序是
  中心 → 平面 → 单轴。
- **arcball**：屏幕偏移映射到单位虚拟球（球外投到赤道），`angle = atan2(|p0×p1|, p0·p1)`，
  轴从相机系（`right/up/-fwd`）转到世界系；自由旋转**固定按世界空间**施加。
- **平面拖拽必须兜底**：射线几乎与平面平行时交点跑无穷远、位移瞬间爆炸 →
  `kMaxDragUnits = 2000`，超了就丢弃该帧。
- **本引擎欧拉序是 `R = Rx·Ry·Rz`**，而 `glm::eulerAngles` 用的是 **YXZ**，语义不同**不能直接用**
  → 自己写 `eulerFromRotation()`。GLM 列主序下 `R_ij = m[j][i]`：
  `y = asin(m[2][0])`、`x = atan2(-m[2][1], m[2][2])`、`z = atan2(-m[1][0], m[0][0])`，
  `|sy| > 0.99999` 走万向锁分支（x/z 只有一个自由度，全归到 x）。
- API：`hotHandle()/handleName()`（**旧的 `hotAxis()/axisName()` 已删**）。

### 缩放：拖的那根轴 ≠ 改的那个分量（2026-10-10 修）
**用户报的症状**："切到 Scale 拖 Y 轴却在 Z 方向拉伸、拖 Z 轴却在 Y 方向拉伸"。
- **根因**：`TransformComponent::scale` 是**局部**的（`localMatrix = T·Rx·Ry·Rz·S`），缩放作用在
  **物体自己的三根轴**上；而手柄拖的是**世界**里的方向。物体一转两套轴就不重合，而旧代码
  `sc[axisIdx] = m_startScale[axisIdx] * factor;` 直接把世界倍率塞进局部分量 → 必然写错维度。
- **修法**：把拖拽方向**投影到局部轴**再分配
  ```cpp
  for (i) w[i] = |dot(axis, localAxisWorld[i])|;      // 单轴手柄
  for (i) w[i] = |dot(U, L[i])| + |dot(V, L[i])|;     // 平面手柄（两条腿）
  sc[i] = m_startScale[i] * (1 + (factor - 1) * w[i]);
  ```
  轴对齐时 w 退化成 (1,0,0) 之类 → 与旧行为一致；整体等比与朝向无关，保持 `sc = start * factor`。
- `GizmoController` 新增 `m_localAxisWorld[3]`（恒定局部轴）/ `m_dragLocalAxes[3]`（beginDrag 冻结）。
- **该断言的不变量**：不是"拖 Y 改 scale.y"，而是
  **"物体在世界里长大的方向 ≈ 被拖手柄的世界方向"** —— 长大方向 = `Σ(Δscale[i] · 局部轴 i 的世界方向)`。
  见 `tools/verify_gizmo_scale.py`（含"绕 X 转 90° 后拖三根轴"的用例）。
- 坐标系切换钩子：`MYVK_EDITOR_SPACE=world|local`。

## 地平面参考栅格（`assets/shaders/grid.{vert,frag}` + `Renderer::GridSettings`）
- **解析式、零顶点缓冲**：VS 用 `gl_VertexIndex` 拼 4 顶点铺在 **z = `planeOffset`（默认 0）** 的大四边形；
  格线在 FS 里按 `fract(p/step + 0.5) - 0.5` 求"到最近线的距离"，除 `fwidth(p)` 转成**像素距离**
  → 1px 抗锯齿线宽，**不依赖 MSAA**，远处靠 `smoothstep` 淡出避免摩尔纹。
- **三种图层**：最小格（1 m，暗）× `minorStrength` → 主格（10 m，亮，线宽 ×1.4）→
  世界轴（**X 轴红 / Y 轴绿**，与视口 gizmo 同配色；Z-up 迁移后即为此，早期文档写的"Z 轴蓝"已作废）。
  默认 `extent 60 / minor 1 / major 10`。
- **自带地面（2026-10-10 新增）**：`EditorScene::resetToEmptyScene` 会建一个 `Ground` 实体
  （`plane(1.0)` × `scale(120,120,1)`，**castShadow=false**），顶面正好在 z=0 与栅格面重合；
  不透明 Pass 先画地面、栅格 Pass 排在其后 → 栅格线画在地面上、互不遮挡。改地面尺寸/位置时
  **必须同步 `GridSettings::extent`**，否则边缘会出现"线飘在没有地板的地方"。
  **地面是"上锁"的**（`ecs::LockedComponent`）：视口点不到、选中也不给变换手柄 —— 它是参照系，
  不该被平移/缩放/旋转。想调它得先在 Inspector 里取消 `Locked` 勾。详见"编辑器锁定"小节。
- **淡化是"距栅格中心"而不是"距相机"**（2026-10-10 修）：`fade.xy` 与切比雪夫距离
  `max(|p.x|,|p.y|)` 比较，栅格的样子**不随相机移动变化**。旧写法用 `length(vWorld - vCam)`
  = 一圈"没有栅格的环"跟着相机跑，飞行时非常显眼（`grid.vert` 的 `vCam` 因此删掉了）。
- **线宽必须按占空比封顶**（`kMaxLineDuty = 0.22`）：线宽是**像素**单位，格子小到亚像素时若不封顶，
  1.4px 的线画在 0.3px 的格子上 → 覆盖率恒为 1 → 整片糊成一条亮带（实测远处整行亮度 +55）。
  `gridCoverage()` 里 `w = min(baseWpx, kMaxLineDuty * cellPx)`，最小格/主格共用；
  亚像素淡出阈值 `smoothstep(0.7, 1.6, cellPx)`（**旧值 2.5~8.0 太狠**：相机贴地 0.4m 平视时
  1m 格在 13m 外就整族消失 —— 这就是"贴近地面看栅格会消失"的根因）。
- **与地面共面必须压 z-fighting**：VS 里 `clip.z -= 1e-6 * clip.w`（等价于全距离恒定的 NDC 偏移）。
- 管线：`TRIANGLE_STRIP` / 无顶点布局 / `depthTest=true` / `depthWrite=false` / `blendEnable=true` /
  `cullMode=NONE`。推常量 48B（`params/color/fade`）。
- **开关**：`Renderer::gridEnabled()`（`bool&`），由 `EditorApp::onUpdate` 每帧写
  `= isEditing() && ctx.showGrid()` —— **Play 期间强制关**（视口是游戏画面）。
  UI 三处等价入口：视口右上角 HUD 勾选框 `Ground grid (1 m)`（**默认开**）、View 菜单、`MYVK_GRID=0`。
- 数值调参在 `drawEnginePanel()` 的 `Viewport grid (z = 0)` 段与 Preferences 的 `Ground grid (z = 0)`
  段（两处等价；`Edge fade start/end (m)` = **距栅格中心**的距离，见上。开关**刻意不放**在那里）。
- ⚠️ 栅格是**世界空间**的，与相机无关；相机在水面/地板上方才能看到它，且会被墙正确遮挡。

## 全屏与窗口几何

### 全屏（两种，别再混）
- **Play 态全屏 = 窗口真全屏 + 视口独占整屏**：`EditorContext::m_gameFullscreen` 只存**逻辑**
  状态（本类不认识 Window），真正的窗口切换在 `EditorApp::syncWindowFullscreen()`
  → `Window::setFullscreen(true)` → `glfwSetWindowMonitor(win, primaryMonitor, 0,0,
  mode->width, mode->height, mode->refreshRate)`。退出时回到**进入全屏前记住的窗口几何**。
  快捷键 **F 切换** / **Esc 退出**（都只在 Play 态生效）；按钮两处：主工具栏（`isEditing()` 时禁用）
  + 视口右上角播放浮层（全屏时工具栏不画，浮层那个是唯一图形化退出口）。
- **F11 = 只切窗口全屏**（面板照旧），任何模式都可用；View 菜单也有
  （`EditorContext::windowFullscreenRequest()` 请求标志 → EditorApp 消费，和 `quitRequested` 同套路）。
- **切换必须做边沿检测**：`EditorApp::m_appliedWindowFullscreen` 记住"上次告诉 GLFW 的值"，
  只在变化时动窗口。若每帧无条件 `setFullscreen(want)`，编辑态下 `want=false` 会把 F11
  打开的纯窗口全屏立刻关掉；F11 在"只看游戏"模式下也会看起来像按键失灵。
- `Window::setFullscreen` 里除 `glfwSetWindowMonitor` 外**必须再置 `m_resized = true`**：
  framebuffer 回调通常会置位，但漏掉时会拿着旧尺寸的交换链渲染一整帧（画面被拉伸）。

### 窗口几何：首次居中 70%，之后记住上次关闭时的样子
- `engine/core/WindowGeometry.{h,cpp}`（新）：`applyStartupGeometry(window, fraction, 存档文件, 兜底W,H)`
  + `saveWindowGeometry(window, 存档文件)`。
  - 无存档 → 主显示器**工作区**（`glfwGetMonitorWorkarea`，已排除任务栏）居中、宽高 = 工作区 × fraction。
  - 有存档 → 用存档（含 `maximized`）；**必须先做可见性校验**（`visibleOnSomeMonitor`），
    换显示器 / 拔外接屏后存档可能落在屏幕外 —— 那样窗口开了也看不到、拖不回来。
- `EditorApp`：存档名 `editor_window.ini`（和 `imgui.ini` 一样放 CWD）、`fraction = 0.70f`；
  `onInit()` **第一件事**就落位，`onShutdown()` 保存。
  - `core::Application` 为此新增 `virtual void onShutdown()`（主循环退出前、窗口还活着时调用）。
  - 落位走的是"置 resize 标志 → Renderer 下一帧重建交换链"，和用户手动拖窗口同一条路。
  - **自动化跑不写存档**：`MYVK_FRAMES` 或 `MYVK_NO_WINDOW_SAVE` 存在时跳过
    （`automationRun()`）；`tools/capture_window.py` 默认给子进程注入 `MYVK_NO_WINDOW_SAVE=1`
    —— 它会故意缩放窗口压测，否则会把用户的存档改成压测尺寸。
- `Window` 新增的几何 API：`windowPos/windowSize/setWindowPos/setWindowSize/maximized/maximize/
  primaryMonitorWorkArea/windowedGeometry`。注意**几何是屏幕坐标**（glfwGetWindowSize），
  与 `framebufferSize`（像素，高 DPI 下不同）不要混用。`windowedGeometry()` 在全屏时返回
  "进入全屏前"那份 —— 否则下次启动会拿全屏尺寸当窗口尺寸。

## venn 命名与图标（三层落地）
引擎/编辑器统一叫 **venn**：窗口标题 `Venn Editor`（**Sandbox 已删，没有第二个窗口了**），
日志头 `=== Venn Engine - Editor ===`。图标形象是**滑稽可爱豚鼠**（矢量卡通风：
奶油色身体 + 焦糖色斑块、大眼、龅牙、抿嘴笑、炸毛），源图在 `design/venn_logo_source.png`。

三层各自独立、缺一不可 —— **2026-10-10 晚重做了第 2 层**（原来的实现有 bug，见下）：
1. **exe 资源**：`assets/icons/venn.rc`（`1 ICON "venn.ico"` + `VERSIONINFO`）+
   CMake `enable_language(RC OPTIONAL)` → 资源管理器图标。`venn.ico` 含
   16/24/32/48/64/128/256 七档（全 PNG 压缩）。
2. **窗口图标**：`Window` 构造函数里 `GLFW_VISIBLE=FALSE` 建窗 →
   `applyIconFromResource()` → 再 `glfwShowWindow`。`applyIconFromResource()`
   从 exe 自己的 `RT_GROUP_ICON #1` 用 `LoadImageW(mode, 1, IMAGE_ICON,
   SM_CXICON/SM_CXSMICON, ...)` 各取一张，然后**三处都设**：
   `WM_SETICON`(ICON_BIG/ICON_SMALL/ICON_SMALL2) + `SetClassLongPtrW`
   (GCLP_HICON/GCLP_HICONSM)。析构 `DestroyIcon`。
   `setIconFromFile()` 退化为**兜底**：`m_iconFromResource` 为真时直接 early-return。
3. **AUMID**：`SetCurrentProcessExplicitAppUserModelID(L"venn.engine.editor")`
   —— 必须在**任何窗口创建之前**调。Editor 是**控制台子系统**程序，不显式设
   AUMID 时 Windows 按 exe 路径猜，可能把窗口和控制台主机算成一组。
4. **PNG 落盘**：`assets/icons/venn_icon.png`（512×512 透明）只作兜底，不再走它。

> ⚠ **不要**退回"单张 512 PNG + `glfwSetWindowIcon`"的写法。GLFW 的
> `_glfwSetWindowIconWin32` 按 `abs(w*h - targetW*targetH)` 选图，只喂一张
> 512 时 `ICON_BIG`(48) 和 `ICON_SMALL`(24) 都拿到 512，再由 GDI 硬缩 → 小尺寸糊；
> 而且它只发 `WM_SETICON`、**不设窗口类图标**，任务栏按钮是在窗口**第一次显示**
> 时创建并记下当时的图标 —— 默认 `GLFW_VISIBLE=true` 会在 `glfwSetWindowIcon`
> 之前就 `ShowWindow`，于是抢跑。详见坑点 #55。

抠图要点：抹水印 → 抠中性浅灰背景（`sat<=10 && dist<=16`）→ 去地面阴影 →
**洪泛填洞还原封闭内部**（否则眼白/牙齿会被一起打掉）→ 腐蚀 1px 去浅色镶边。

## 资产溯源与序列化
Mesh/Material/Texture 各带"来源描述"（`*Source`），序列化只写描述、加载时由 AssetManager 重建
（JSON 可读可手改可进版本库）；glTF 派生资源用"模型键 + primitive 下标"；路径入 JSON 前过
`assets::makeAssetRelative()`；层级父子用**数组下标**；相机与方向光也在同一个 JSON 里。

- **`Mesh` 带局部 AABB 缓存**（`hasBounds()` / `boundsMin()` / `boundsMax()`，`upload()` 时从顶点算一次）：
  阴影正交范围、编辑器拾取/贴地都要"物体实际尺寸"，别再退回实体原点。
- **`Texture` 的色彩空间是契约**：`fromPixels(..., bool srgb)` / `makeSolid(..., uint32_t size, bool srgb)`
  —— albedo/emissive 用 true，**normal/ORM 必须 false**（见坑点 #47）。`m_source.srgb` 记录该选择。
- **材质现在有 6 个纹理槽**（2026-10-10 加）：albedo / normal / orm / **roughnessMap / metallicMap /
  emissiveMap**。后三个是**独立单因子槽**，刻意**不复用 orm**——UE5 语义是每个因子各挂一张图，而
  ORM 是打包图，共用槽会让"只想换粗糙度"变成"必须提供打包图"。
  - 合成方式：roughness/metallic **相乘**（默认绑**纯白** → 恒等）；emissive **相加**（默认绑**纯黑** →
    恒等）。**"加白"是错的**：会让所有没绑自发光图的材质整片发白光。`Renderer::m_defaultBlack` 为此存在。
  - 描述符集数量 `kTextureSetCount` 3 → **6**，主前向管线与栅格管线的 `setLayouts` 都从 4 扩到 **7**
    （字段数不一致 → VUID-...-08600）。新增 set = 4/5/6，见 `assets/shaders/pbr.frag`。
  - `SceneSerializer` 读写 `roughnessMap/metallicMap/emissiveMap`；读盘后必须**显式补写**
    `mat->xxxMap`（`makeMaterialPBR` 按名字命中缓存时**不会**碰这几个新字段）。
  - ⚠ 落盘的 `name` 就是纹理**缓存键**（`resolveTexture`：`key = name.empty() ? path : name`），
    所以数据贴图的 `#linear` 后缀**不能剥**——剥了会让"同一张图当颜色/数据贴图"在重载时撞键。

## 已知坑点（血泪清单）
1. PowerShell 里 `-DXXX=3.5` 会被拆开 → **必须加引号**。
2. `Window.h` 里 **vulkan.h 必须在 glfw3.h 之前**（`glfwCreateWindowSurface` 被 `#if VK_VERSION_1_0` 包着）。
3. 每张交换链图像需**独立**的 render-finished 信号量；析构时 Swapchain 必须先于 Surface 销毁。
4. imgui master(≥1.93) InitInfo 变了（RenderPass 移到 `PipelineInfoMain.RenderPass`）；无 docking 分支
   → `ImGuiWindowFlags_NoDocking` **未定义**。
5. tinyobjloader 字段 `config.vertex_color`；tinygltf master 已重构 → 固定 v2.9.4。
6. **深度图当纹理采样**必须用 `DEPTH_STENCIL_READ_ONLY_OPTIMAL`（非 SHADER_READ_ONLY）。
7. **glTF ORM 陷阱**：`metallicRoughness` R 通道常写 0，本引擎 ORM 约定 R=AO → 必须 CPU 侧重打包。
8. 全屏三角形用 `gl_VertexIndex`（Vulkan GLSL），不是 `gl_VertexID`。
9. `World::clear()` 归零 generation → 旧句柄可能"碰巧" alive；`Scene::clear()` 必须重置 `m_lightEntity`。
10. **本环境 PowerShell 输出不回显、bash coreutils 不可靠** → 结果写文件再 Read/Grep；日志用
    `[System.IO.File]::WriteAllText(p,s,UTF8Encoding($false))`；长命令用后台任务（其输出可被捕获）。
11. `enum class` 底层类型必须是整型；嵌套类型当类型名用要写全限定名（`Scene::LightStats`）。
12. **头文件前向声明的类型，定义不能放进匿名命名空间**（否则是另外两个类型，`StmtPtr/ExprPtr` 对不上）。
13. `std::filesystem::path` **不能**直接赋给 `std::string` → 用 `.generic_string()`。
14. 引号 include 先查**当前文件所在目录** → `editor/script/*.cpp` 里写 `#include "ScriptEngine.h"`。
15. **同一文件的多处修改必须串行下发**（并行 Edit 第二个会静默回滚第一个）。不同文件可并行。
16. **"编译通过 + 日志无错" ≠ 画面正确**，一定要看截图。
17. 相机视线要避开柱子（柱子在 (±4,±4)，正对角视线必穿 (4,4)）；默认 editor 相机 distance=12/pitch=0.28
    会从盒外看房间外墙 → 一片暗灰。
18. `ImGui::BeginDragDropSource()` 默认要求上一个 item 有非零 ID → 无 ID 的 item 必须传
    `ImGuiDragDropFlags_SourceAllowNullID`，否则 `IM_ASSERT(0)` → 退出码 `0xc0000409`。
19. **ImGui 每帧把聚焦窗口提到最前** → 想让视口上的 HUD/覆盖层可见，**必须是 viewport 的 child window**
    （`SetCursorScreenPos` 定位 + 在 `ImGui::End()` 之前画）；独立的顶层窗口会被 3D 图盖住。
    （旧坑：CreateProcess 的 `bInheritHandles` / 环境块继承 —— 跨进程方案已删除，见 editor/ 一节。）
20. MinGW 的 `os_defines.h` **已定义 `NOMINMAX`** → 自己再 `#define` 会告警重定义。
30. **ImGui 的 popup 必须"每帧都被提交"，否则帧末会被直接关掉**：菜单一弹出来就正好
    盖在光标下，触发它的那个 item 立刻不再 hovered —— 若把 `BeginPopup*` 塞进
    `if (hovered)` 里，下一帧不再执行、popup 没提交，菜单**只闪一帧**就消失。
    正解：`drawEntry` 里只置"请求"（记下目标快照），`BeginPopup` 放到循环**之外**每帧
    无条件调用；且 `OpenPopup` / `BeginPopup` 必须在**同一 ID 栈深度**（同一窗口、无 PushID），
    否则 `GetID()` 哈希不同，永远打不开。
31. **自动化输入：扩展键（Delete/方向键/Insert/Home/End）用 `keybd_event` 必须同时给扫描码**。
    GLFW 从 `lParam` 的 `(KF_EXTENDED|0xff)` 取 scancode 再查 `keycodes[]`，Delete 的表项是
    `keycodes[0x153]`。只传 `KEYEVENTF_EXTENDEDKEY` 而 `bScan=0` → 系统回退 `MapVirtualKey(VK)=0x53`，
    扩展位丢了 → 查表得 0 → **键被整个丢弃**（现象：F2 有效、Delete 死活没反应）。
    正解：`keybd_event(vk, MapVirtualKeyW(vk,0), extended?0x0001:0, 0)`。
32. **这台机器上鼠标会被外部挪动**：`SetCursorPos` 之后 0.35s 内光标会平滑漂走
    （实测采到 `(255,750)→(313,817)` 这种曲线），而空闲时又完全静止。
    引擎里没有任何 warp 光标的代码。所以自动化点击不能"设一次就点"，
    必须**闭环**（连续两次读回都在 ±2 内）+ 点击后校验画面再重试。
33. **调试日志要限流**：把 `logRect(tag,...)` 放进递归的 `drawNode` 里，同一 tag 每帧被调
    N 次、值一直变 → 两分钟写了 30MB，而且脚本取"最后一行"拿到的是随机某一行（坐标全错）。
    必须"每帧只记一次"。
34. **ImGui 颜色经 SRGB 交换链后会明显变亮**：`ImVec4(0.62,0.24,0.22)` 在屏幕上约是
    `(206,140,133)` 的**浅鲑红**。按 `*255` 去写像素判据（找红色按钮）会一个像素都匹配不到。
35. **自动化测试用的"红色确认按钮"定位**：用"新旧两张图里**新增**的红色区域"，能天然排除
    3D 场景里本来就有的红盒子。
36. **改 exe 图标要在 CMake 里 `enable_language(RC OPTIONAL)`**：MSYS2 ucrt64 自带
    `windres`；`if(CMAKE_RC_COMPILER)` 判可用后再把 `.rc` 加进 `target_sources`。
    新增后**必须重新 configure**（CMakeLists 变了会自动触发）。
37. **`SetForegroundWindow` 会静默失败，而窗口不在前台时 ImGui 的拖拽完全收不到输入**。
    Windows 的硬规则：只有**当前前台进程**（或被它启动的进程）才允许改前台窗口。
    症状极具迷惑性 —— 坐标对所有、`SetCursorPos` 返回 1、`mouse_event` 也不报错，
    但拖拽中间态截图里**连跟随提示都没有**，`BeginDragDropSource` 一次都没点火。
    绕法：先按下再松开一次 ALT（`keybd_event(VK_MENU,…)`），系统就认为"用户在操作"，
    随后的 `SetForegroundWindow` 才会被放行 —— 见 `tools/verify_asset_drag.py::force_foreground`。
38. **解析自家日志的正则别用行首锚定**。日志行有 `[INFO ] ` 前缀，写 `^TAG=` 的
    `re.match` 会一条都匹配不到，且**不报错**（脚本静默地"什么都找不到"）。
    用 `re.search`，标签用 `[^=]+` 而不是 `.+`（`.+` 贪婪会把 `=(x,y)-(x,y)` 吞错位）。
39. **改项目根目录名会被"以它为 cwd 的进程"挡住**。`mv` 报
    "另一个进程正在使用此文件" / `System.IO.IOException`，而**子目录改名完全正常**
    ⇒ 锁在根目录本身（VS Code 的扩展宿主以工作区根为 cwd 是最常见的一种）。
    先让用户关掉那个窗口再改；改完必须删 `build/` 重新 configure（CMake/Ninja 缓存里
    全是绝对路径），并同步所有硬编码绝对路径（`.workbuddy/skills/*`、文档）。
21. **ImGui 固定像素尺寸的浮层会被字号缩放裁掉** → 宽度用 `CalcTextSize(...)` 取所有文本的最大值、
    高度用 `GetFrameHeightWithSpacing()/GetTextLineHeightWithSpacing()/WindowPadding` 累加。
    （`fontScale` 是用户可调的，实测写死 236px 时提示行被裁成半截。）
22. **`Camera` 飞行模式的 setter 不变式**：`m_target` 与 `m_flyPos` 互为逆运算，任何只改一边的
    接口都会让另一种模式"拖了没反应"。新增接口务必按 `m_fly` 分支（详见"视口相机与导航"）。
23. **视口工具栏已经很长**：往里加按钮会被挤出右边缘裁掉（实测 `Fullscreen (F)` 变成 `Fulls…`）。
    再加控件前先想"能不能挪到浮层 / 主工具栏"。
24. **验证"输入驱动"的行为别只靠像素差分**：编辑器场景是大片平滑渐变，轻微视角变化就会让
    60%+ 像素变化 → 差分恒等于"全变了"，没有区分度。可靠读数是**界面上的数字**：
    Inspector 的 `Target/Yaw/Pitch` 与 HUD 的 `Sensitivity`（截图后直接读数比对最实在）。
    注入真实输入：`SetCursorPos` 让窗口收到**真**的 WM_MOUSEMOVE（只 PostMessage 会被真实鼠标
    消息覆盖 → 视口不再 hovered），再 `PostMessage(WM_MOUSEWHEEL / WM_*BUTTONDOWN)`。
    ⚠️ **WM_MOUSEWHEEL 的 wParam 高字是"这一次的 delta"（WHEEL_DELTA=120），不是累计格数**；
    传累计值会让效果成倍（第一次就踩了：3 格变成 9 格）。
25. **窗口几何存档 vs 自动化**：`tools/capture_window.py` 会故意缩放窗口压测 —— 让它给子进程
    注入 `MYVK_NO_WINDOW_SAVE=1`，否则用户的"上次窗口大小"会被压测尺寸覆写。
    另外该工具用 `terminate()` 强杀进程，`onShutdown()` 不会执行（这也是为什么它是安全的）。
26. **"方向反 + 越拖越快"是两个独立缺陷叠加**（符号取反 × 自激正反馈），只修一个会得到
    "方向对了但仍在加速"。数值可复现的 bug **先用小脚本把公式跑一遍**再动 C++，
    比反复截图快得多。**验证用的环境变量钩子用完必须删干净**（`grep -rn "TEMP-"` 收尾）。
27. **改某条管线的 push constant range = 有可能打断整个 Pass 的 set 0 绑定**
    （VUID-...-08600，报错会出现在**后续无关的 draw 上**，极易误判）。想给旁路管线"缩 range"
    之前先读"渲染管线约定"最后一条。
28. `ContentBrowser` 面板会**扫描进程 CWD 的所有图片**做缩略图 → 在项目根目录随手丢验证截图
    会被当成资源"加载"（日志刷 `Texture loaded:`）。验证产物请用固定前缀并及时清理。
29. **"验证层静默"要当成一等验收项**：`grep -c "VUID\|ERROR"` 必须为 0。校验信息在
    `capture_window.py` 的 stdout 里，很容易被"截图看着没问题"盖过去。
40. **logRect 矩形日志的正则要允许小写**：tag 带 `Mesh`/`Cube` 这类小写时，
    `[A-Z-]+` 会**静默失配**——"转储里明明有、脚本就是等不到"（子串转储看得见、
    正则抓不到）。脚本侧用 `[A-Za-z-]+`。
41. **`ImGui::BeginMenu` 只有子菜单展开才返回 true**：给它配的 logRect 必须写在
    if **之外**无条件打（BeginMenu 无论返回值都提交 item，GetItemRectMin/Max 仍有效）。
    写在 if 里 = 悬停前矩形永远不出来，自动化"等父项矩形才知道悬停哪"直接死锁。
42. **自动化注入 Shift+A 的唯一稳法**：ALT-trick 前台确认（force_foreground）+
    keybd_event **真实 Shift**（PostMessage 的 Shift 进不了 GetKeyState，GLFW
    `getMods()` 读不到）+ **PostMessage 带扫描码的 A**。全真实 keybd_event(Shift+A)
    反而不稳（脚本后台跑时前台策略会拒绝）。
43. **自动化启动别读窗口几何存档**：存档可能是上次手动会话留下的全屏态，首帧
    Appearing 窗口（Preferences）按全屏居中，脚本随后缩窗 → "飞到"客户区角落。
    `automationRun()` 时**跳过 `applyStartupGeometry`（读和写都跳）**。
    另：`Begin("X###id")` 的窗口 ID 是 ### 后面的部分——换 ID 才能甩掉
    imgui.ini 里旧窗口的位置遗产。
44. **`glm::ortho` ≠ Vulkan 深度**：GLM 默认（未定义 `GLM_FORCE_DEPTH_ZERO_TO_ONE`）输出
    OpenGL 约定的 z∈[-1,1]，Vulkan 只保留 clip z∈[0,1]。主相机的 `glm::perspective` 因为
    "写和读用同一套"所以看不出问题，但**任何把 z 存进贴图再手工换算的地方都会错**
    （阴影图就是）。这类地方必须用 `glm::orthoRH_ZO` / `perspectiveRH_ZO`，且采样侧不要再
    `*0.5+0.5`（那只对 xy 成立）。
45. **包围盒别只用实体原点**：`worldMatrix(e)[3]` 丢掉尺寸/缩放，单个物体 → 半径 0。需要
    "物体实际多大"就用 `Mesh::boundsMin/boundsMax`（上传时算好缓存，8 角过世界矩阵）。
46. **本引擎是 Z-up**：着色器里凡是要"上/下"的都得用 `.z`（`envColor` 曾错用 `.y` → 环境光
    梯度沿世界 +Y 走，地面出现一条明暗硬边）。同理任何 `dir.y * 0.5 + 0.5` 的天空梯度写法都要复核。
47. **数据贴图必须 UNORM**：normal / ORM 用 `srgb=false`，颜色贴图才 `srgb=true`。
    `Texture::makeSolid` / `fromPixels` 的 srgb 默认是 **true** —— 建数据贴图时必须显式传 false。
48. **改了 GLSL 之后 `make build` 不一定把 .spv 拷到 `build/bin/shaders/`**：拷贝是 Editor 目标的
    POST_BUILD 步骤，**只有 Editor 重新链接才会跑**。只改着色器时 ninja 只跑 `glslc`，
    `build/bin/shaders/*.spv` 仍是旧的 → "改了没效果"。对策：`cp build/shaders/*.spv build/bin/shaders/`
    （或顺手 touch 一个 .cpp 让它重链）。**验证着色器改动前先核对两边时间戳。**
49. **mingw 的 `-Wformat` 不认 `%zu`**：会当成无此转换 → 报"参数过多/类型不符"。
    size_t 打日志用 `%llu` + `static_cast<unsigned long long>(...)`。
50. **"视角相关"的淡出阈值要按屏幕密度算，别用固定距离**：栅格最小格原本"一格剩 2.5px 就整族抹掉"，
    而一格占几像素 = 格距 / `fwidth` 是**随视角**变的 —— 相机一贴近地面，1m 格在十几米外就全没了
    （"贴近地面看栅格会消失"的根因）。阈值要放到真正的亚像素区（≈1.5px），并且**先把线宽按占空比
    封顶**（否则亚像素时线宽占满整格，整片糊成亮带）。
51. **像素单位的线宽一定要封顶**：`min(baseWpx, kMaxLineDuty * cellPx)`。只按像素给线宽、不设上限，
    远处必然糊成一片亮色（覆盖率恒 1）。这条对任何"解析式图案/描边"都成立。
52. **别用"到相机的距离"做固定物体的淡出**：栅格是固定的一块地，用相机距离淡出等于让一圈
    "没有栅格的环"跟着相机跑。固定物体的可见范围要用**它自己的坐标**表达（这里是到栅格中心的
    切比雪夫距离），这样外观与相机无关。
53. **环境的钩子要注意初始化顺序**：`MYVK_EDITOR_CAM` 必须放在 `MYVK_EDITOR_SCENE` **之后** ——
    加载场景会连相机一起覆盖，顺序反了钩子就白设（表现为"机位没生效"，且不报错）。
54. **"可被选中/可被变换"要显式声明**：靠约定（"别去点地面"）守不住。用标记组件 + 在
    **拾取和手柄两处**都挡（只挡一处，从层级树选中就绕过去了）。加新的"不可交互"实体时同理。
55. **Windows 窗口图标要"多尺寸 + 显示前设好 + AUMID"三管齐下**：GLFW 的
    `chooseImage` 按 `|w*h - target|` 挑图，单张 512 → ICON_BIG(48)/ICON_SMALL(24)
    都拿到 512 再被 GDI 硬缩（小图标糊）；`glfwSetWindowIcon` 只发 `WM_SETICON`、
    **不设窗口类图标**；任务栏按钮在窗口**第一次显示**时创建并记下当时图标，而
    `GLFW_VISIBLE` 默认 true → `glfwCreateWindow` 内部先 `ShowWindow` 抢跑。
    正解：`GLFW_VISIBLE=FALSE` → 从 exe 的 `RT_GROUP_ICON` 用 `LoadImageW` 按
    SM_CXICON/SM_CXSMICON 各取一张 → `WM_SETICON`×3 + `SetClassLongPtrW`×2 →
    `glfwShowWindow`；再在最前面 `SetCurrentProcessExplicitAppUserModelID`
    （控制台子系统程序尤其要显式设）。详见"venn 命名与图标"一节。
56. **`logRect` 是按矩形去抖的 → 弹出物的 tag 必须带"第几次打开"的序号**：
    `VP-ADD-Mesh` 这种固定 tag，第二次在**同一位置**打开菜单时矩形没变、一行都不打，
    自动化脚本于是拿到上一轮的陈旧坐标、在子菜单展开前就点下去。症状极具迷惑性：
    **第一次加 Cube 成功、紧接着加 Sphere 必失败**（新 tag 的 PointLight 反而正常）。
    修法：`ViewportPanel::drawAddMenu` 里 `++m_addMenuSeq`，tag 变 `VP-ADD-Mesh#3`；
    脚本侧按 `tag(#\d+)?=` 取**最后一条** = 最新一次。
57. **GLFW 的键值按 scancode 查表 + 修饰键走 GetKeyState**：注入按键必须带真实扫描码
    （`PostMessage(hwnd, WM_KEYDOWN, vk, (sc<<16)|1)`），否则映射成 `GLFW_KEY_UNKNOWN`；
    而 Shift/Ctrl 这类修饰键**必须用真实 `keybd_event`**（PostMessage 的修饰键
    GLFW 看不见 —— 实测窗口已在前台也一样失败）。所以"Ctrl+S""Shift+A"只能
    真实修饰键 + PostMessage 字母键混合注入。
58. **自动化脚本里 `SetForegroundWindow` 要加 attach-thread-input 兜底**：只有当前前台
    进程才允许改前台窗口，后台脚本直接调会**静默失败**；症状是"脚本单独跑全过、跟在别的
    命令后面连着跑就偶发全挂"。最可靠的一条是把调用线程的输入队列临时挂到当前前台线程
    （`AttachThreadInput(me, fg, TRUE)` → `SetForegroundWindow` → `AttachThreadInput(..., FALSE)`），
    ALT 点一下只当兜底。这个 helper 统一放 `tools/verify_asset_drag.py::force_foreground`，
    **别在别的脚本里复制一份**（复制的版本会落后于修复）。
59. **`ImGui::EndGroup()` 会 `ItemAdd`**（带一个自动 ID），而 `BeginDragDropTarget()` 在
    `LastItemData.ID==0` 时会用 `GetIDFromRectangle()` 兜底 —— 所以**一整组的包围盒可以直接
    当 drop target**，不需要额外塞 `InvisibleButton`。`SetDragDropPayload` 的 type 上限 32 字节。
60. **同一张图片在不同色彩空间下必须是两份 GPU 纹理 → 缓存键要编进 sRGB**：`AssetManager`
    只按名字缓存，albedo(`_SRGB`) 和 normal(`_UNORM`) 共用一个键时第二次会拿回第一次那张，
    法线被 gamma 过一次 → 光照方向全歪且极难查。本工程的约定是
    `slotCacheKey(rel, srgb) = srgb ? rel : rel + "#linear"`（`#linear` 只是内部标记，
    **显示前要剥掉**，见 `stripVariantSuffix()`）。⚠ 但**落盘的 `name` 字段不能剥** ——
    它就是 `resolveTexture` 用的缓存键（`key = name.empty() ? path : name`），剥了重载时会撞键。
61. **"因子 × 贴图"的默认值必须是数学中性值**：roughness/metallic 贴图与因子**相乘** → 未绑定时
    要绑**纯白**；emissive 贴图与因子**相加** → 未绑定时要绑**纯黑**。用"加白"会让所有没绑
    自发光图的材质整片发白光（而且看起来像"曝光坏了"，很难联想到贴图默认值）。
62. **`ImGui::Dummy()` 天然就是拖放落点**：它给 `ItemAdd` 传的 ID 是 **0**，而
    `BeginDragDropTarget()` 在 ID 为 0 时会退回 `GetIDFromRectangle()`，于是"这块矩形"就是热区，
    **不需要额外的 `InvisibleButton`**。配合 `AddRectFilled/AddRect/AddImage/AddText` 就能手绘一个
    "小方块 + 加号 → 绑图后长成缩略图 + 文件名"的槽位（见 `InspectorPanel::drawTextureChip`）。
63. **`ImGui::SameLine(offset)` 的 offset 是"相对窗口内容起点"**，不是相对光标
    （imgui.cpp 注释：`offset_from_start_x`）。想要"标签右边自然排一个控件"就直接
    `SameLine()` 不传参，宽度用 `GetWindowContentRegionMax().x - GetCursorPosX()` 算。
    **别把窄面板里的槽位钉成固定列**：Inspector 默认只有 ~315px，"定宽因子控件 + 标签 + 槽位"
    三样塞不下 → 标签被压扁、槽位叠到控件上。

## 工具
- `tools/capture_window.py`：启动 exe → 缩放压测 → `PrintWindow` 截图（不受遮挡；进程需 DPI 感知）。
  参数 `--no-resize / --size WxH / --out / --wait / --env K=V / --exe / --title / --args`。
  **默认目标已经是 `build/bin/Editor.exe` / 标题 "Venn Editor"**（Sandbox 已删）。
  窗口查找用 `EnumWindows` + 标题**前缀**匹配。
  **会给子进程注入 `MYVK_NO_WINDOW_SAVE=1`**（压测会缩放窗口，别污染窗口几何存档）。
  抓 Play 态画面：`--env MYVK_EDITOR_PLAY=1`；**想抓全屏布局**得改 `EditorApp::onInit()` 里
  `m_ctx->gameFullscreenRef() = true;` 临时置位（无命令行开关），抓完记得撤。Play 态 HUD 在视口右上角。
  运行：`& "C:\Users\v_lweiili\.workbuddy\binaries\python\envs\default\Scripts\python.exe" tools\capture_window.py`
  **注意**：改了面板默认落位要先删 `build/bin/imgui.ini`（`FirstUseEver` 只在无记录时生效）；
  想回到"首次启动的 70% 居中"要删 `build/bin/editor_window.ini`。
- `tools/verify_asset_drag.py`：**真鼠标**端到端拖拽验证（需求 3 就是靠它验的）。
  带 `MYVK_LOG_RECTS=1` 启动 → 读 `CB-CELL <文件>` / `VP-RECT` 矩形 → 摆成固定尺寸 → 强制前台 →
  `SetCursorPos` + `mouse_event` 拖拽 → 截图 + 断言 `alignImportToGround` 的 `bottom == 0`。
  `--asset / --out / --debug-shot / --size / --env`。**改任何拖拽交互都先跑它。**
- `tools/verify_add_menu.py`：Shift+A 菜单 + 播放条端到端（断言 1a/1b/2 布局、3 Cube、
  4 Sunlight；混合注入见坑点 #42，日志正则见坑点 #40）。
- `tools/verify_prefs.py`：Preferences 居中截图验证（`MYVK_EDITOR_PREFS=1` 钩子）。
- `tools/verify_drop_to_floor.py`：**数字键 0 落地 + 地面锁定**端到端（真左键点选、真按键）。
  三个用例：A 守卫（`MYVK_EDITOR_SELECT` 非视口选中 → 按 0 必须 skipped）；
  B 正例（`MYVK_EDITOR_CAM` 把相机对准悬空立方体 → 点中 → 按 0 → 断言 dz=-2.5 / x·y 不变 /
  bottom=0 / 落点 z=0.5）；C 锁定（选中 Ground 不给手柄、点空地无选中）。
  断言全靠日志：`drop-to-floor:`、`pick:`、`gizmo: '...' is locked`。
  **改拾取 / 手柄 / 键盘快捷键都先跑它。**
- `tools/verify_nav_gizmo.py`：导航球 + 视图快捷键。⚠ 2026-10-10 修过两处**假失败**：
  ① `keybd_event(vk, 0, ...)` 扫描码为 0 → 键根本发不出去（改成 `post_key` + 真扫描码，
  见坑点 #57）；② 靠"扫截图里的蓝色像素"找导航球，误命中 Content 面板的蓝像素 → 改为读
  编辑器新打的 `NAV-BALL+X/Y/Z` 矩形。
- `tools/verify_material_drag.py`：**从 Content 拖纹理到材质槽**端到端（UE5 那种绑定）。
  三条断言：① 拖到 albedo 且 `srgb=1`；② 同一张图拖到 normal 且 `srgb=0`；
  ③ **同一张图在两个槽上是两份不同色彩空间的纹理**（若共用缓存键会出现两次 srgb=1，见坑点 #60）。
  断言靠 `material slot '%s' <- %s (srgb=%d)` 日志 + `MAT-SLOT <槽名>` 矩形。
- `tools/verify_material_chips.py`：**Material 面板六个"属性 + 小方块"纹理槽**端到端（2026-10-10）。
  断言：六槽都在 / 空槽是 22px 小方块（不是一整行） / 空槽 `MAT-CHIP` 为 `(empty)` /
  拖拽绑定且 srgb 正确 / **方块里显示的是文件名**（允许左侧 `...`）/ 绑定后宽度 > 60px /
  不越出面板右缘 / **emissive 绑图后视口平均亮度上升**（证明 set 6 真进了着色器）。
  ⚠ albedo 例外：空场景 Ground 的材质本来就带默认底色图（`_default_tex`，纯浅灰），
  它上屏就是"已绑定"、宽 114px —— 别把它当假失败。
- `tools/verify_material_roundtrip.py`：**新增三个槽的存盘/读盘闭环**（2026-10-10）。
  拖三张图 → Ctrl+S → 读临时 json 断言 `*Map.kind=file` + 路径 + srgb(0/0/1) →
  重开载入 → 三个槽又显示回文件名且宽度回到"已绑定"的样子。
- `tools/verify_gizmo_scale.py`：**缩放轴正确性**端到端（2026-10-10）。四用例：
  A 未旋转/世界（三轴分量对位）、B 绕 X 转 90°/世界（`|cos|=1.0000`，用户报的场景）、
  C 绕 X 转 90°/局部、D 平面手柄（平面内两维变、法线维冻结）。
  ⚠ 抓取点必须取**轴端**（`GRAB_T = 1.0`）：`hitTest` 判定顺序是 平面 → 单轴，而透视下
  "62% 的世界长度"在屏幕上的占比会超过 62%，取 0.75 时实测抓 Y 轴抓到了 XZ 方片。
- `tools/verify_mesh_params.py`：**Mesh / Light 差异化参数 + 拖拽调值**端到端。
  靠面板里的**签名日志**做结构性断言（截图证不了"Sphere 比 Cube 多两个控件"）：
  `MESH-PARAMS kind=Sphere labels=Radius,Segments,Rings key=<缓存键>`、
  `LIGHT-PARAMS type=Spot labels=...`。再按 `MESH-PARAM <控件名>` 矩形真拖，
  断言 `primitive rebuild: ... -> N verts (key ...)` 的**值变了 + 缓存键变了**。
  `INS-MESH-HEADER` 是 Mesh 标题栏（Mesh 段默认**折叠**，脚本得先点开）。
- `tools/verify_primitive_roundtrip.py`：**图元参数存盘/读盘闭环**。空场景加 Cube →
  拖 Size → Ctrl+S → 直接读临时 json 断言 `primitive{...}` → 重开并载入 → 断言
  `MESH-PARAMS ... key=builtin/cube/s1.9000` **还是**那个键（= 几何是用存档参数重建的）。
  临时场景放 `tempfile.mkdtemp()`，不往仓库写。
- **`MYVK_SHADOW_DBG=1`**：`Renderer::drawFrame` 每帧打一行
  `SHADOW-DBG casters=N center=(x,y,z) radius=R dir=(x,y,z)` —— 排查阴影问题第一步就看它
  （"没有投影物体却有暗区" ⇒ 与阴影无关；"radius 只有 2" ⇒ 包围盒退化成原点）。
- **着色器问题定位法**（2026-10-10 修"四边形阴影"时总结）：把中间量当 `outColor` 直接输出做二分
  （一次跑一张三通道探针图，如 `vec4(NdotV, specDir.z, N.z, 1)`）；再用**常量输出**
  `vec4(0.5,0.5,0.5,1)` 排除后处理/帧缓冲。图里看不出结论就**转数值**：逐行取中位数亮度
  （避开栅格线），找"最大单步跳变"的位置与幅度 —— 比肉眼判断可靠得多。
  ⚠ 用这个方法前先确认 `build/bin/shaders/*.spv` 是新的（见坑点 #48）。
- `tools/compare_png.py a.png b.png`：逐像素对比。回归时配 `MYVK_FREEZE_ANIM=1 MYVK_HIDE_UI=1`，
  同配置连跑两次应 0 差异。对照实验：`maxLightsPerCluster` 必须 ≥ 灯数（否则参考图自己被截断 = 假差异）。
- **`editor/DebugRects.h` + `MYVK_LOG_RECTS=1`**：把控件真实矩形打一行日志
  （`CB-RECT up=(10,480)-(38,508) center=(24,494)`）。**ImGui 坐标 == 客户区像素，和 PrintWindow
  截图 1:1**，所以自动化脚本可以直接拿这些坐标点控件，不用在截图里"猜"按钮位置（猜坐标踩过坑：
  DPI 缩放 / 窗口被 ini 恢复成最大化 / 用户拖过分隔条，写死的坐标会整体偏掉而且**不报错**，只是点不中）。
  矩形变化超过 4px 才重打一行；同一个 tag 每帧只允许记一次（见坑点 #33）。
  现有 tag：`CB-CELL <文件名>`、`CB-RECT up|reload|cell0`、`VP-RECT`、`HIER-RECT row0`、
  `TR-BAR`（播放条）、`PREFS-WIN`（Preferences 窗口）、
  `VP-ADD-OPEN/WIN/Mesh/Light/Cube/Sphere/Cylinder/Plane/Sunlight/DirectionalLight/SpotLight/PointLight`（Shift+A 菜单，
  **一律带 `#<第几次打开>` 后缀**，见坑点 #56）、
  `NAV-BALL+X/Y/Z`（导航球正轴小球中心）、
  `INS-MESH-HEADER`、`MESH-PARAM <控件名>`、`MAT-SLOT <槽名>`、
  `MAT-CHIP <槽名>`（**方块里当前显示的文件名**，空槽是 `(empty)` —— 让"界面文案"可被断言，
  不用截图认字；`MAT-CHIP` 不打矩形，只打内容）。
  **加了新控件就给它加一个 tag** —— 这是让界面可被自动化验证的唯一途径。
- **面板内容也要能被断言**：控件的**存在性**截图上很难证明（"少了一个控件"看不出来），
  所以 Mesh / Light 段各打一行**签名日志**（只在内容变化时打，不刷屏）：
  `MESH-PARAMS kind=... labels=... key=...` / `LIGHT-PARAMS type=... labels=...`。
  新增/删除一个属性时**顺手更新 labels**，脚本的断言才有意义。
- 启动方式：双击 exe 即可（着色器按 exe 绝对路径找）。查找顺序：`MYVK_SHADER_DIR` → `exe目录/shaders`
  → `cwd/shaders` → `cwd/assets/shaders`。
- **验证产物放项目外**：`/e/code/cpp/_venn_verify/`（截图 / 日志 / 场景 json）。
  删掉的内置 demo / Sandbox 备份在 `/e/code/cpp/_venn_removed_demo/`。
  ⚠️ ContentBrowser 会扫进程 CWD 的图片当资源（见坑点 #28），别往项目里丢截图。

## VS Code（IntelliSense，与 CMake 编译解耦）
手写 `includePath` 必须与 CMakeLists 的 `target_include_directories` 同步；SDK 的 `Include` 根目录
（**不要**加 `Include/glm`，会与 third_party/glm 冲突）；报 "无法打开 vulkan.h" 先怀疑 `.vscode` 配置缺失。
